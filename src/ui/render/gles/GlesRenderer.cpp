// Backend (b): the custom GLES2 renderer on the i.MX6 Vivante GC2000, on the framebuffer ("gl-fb", Vivante
// fbGetDisplayByIndex/fbCreateWindow, vsynced page flips, no X server in the path) or in an X window ("gl-x11").
// EGL/GLES calls go through the platform's GL loader (RendererEnv::loadGl: gldispatch on imx6), bound at run time to
// the matching driver flavour (never two in a process); the native display and window come from
// RendererEnv::nativeWindow (the platform's display code opens X11 or the Vivante framebuffer, this file only
// draws).
//
// Drawing: the whole DrawList becomes one vertex array and ONE glDrawArrays per frame. Rounded rectangles and their
// outlines are quads whose fragment shader evaluates a rounded-box signed distance (anti-aliased, any radius);
// glyphs are quads sampling the 8-bit atlas (GL_ALPHA texture). Client-side vertex arrays, no per-frame allocation.
//
// Damage: the frame diff (ui/render/Damage.h) gives the changed rects; the backend redraws their union
// under glScissor and only builds the quads that touch it. What the back buffer still holds decides how far back the
// damage must reach: EGL_EXT_buffer_age (union of the last `age` frames' damage), else a preserved swap
// (EGL_SWAP_BEHAVIOR_PRESERVED_BIT: age 1), else every changed frame is redrawn in full. A frame without changes is not
// drawn or swapped; render() then sleeps to the next 60 Hz tick so the vsync-paced app loop does not spin.
#include "ui/render/GlyphAtlas.h"
#include "ui/render/RendererRegistry.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <type_traits>
#include <vector>

namespace ui
{
namespace
{
// EGLNativeWindowType is a pointer (Vivante fb) or an integer (X11 Window) depending on the EGL headers.
template <typename T> T toNative(std::uintptr_t v) noexcept
{
    if constexpr (std::is_pointer_v<T>)
        return reinterpret_cast<T>(v);
    else
        return static_cast<T>(v);
}

// Vertex: position (surface px), atlas uv (u < 0: shape), local coords relative to the rect centre, rect half size,
// corner radius, stroke width (0 = fill), colour. 48 bytes.
struct Vertex
{
    float x, y, u, v, lx, ly, hw, hh, radius, strokeW;
    std::uint8_t rgba[4];
    float pad;
};

constexpr const char* vertexShader = R"(
attribute vec2 a_pos; attribute vec2 a_uv; attribute vec4 a_shape; attribute vec2 a_rs; attribute vec4 a_col;
uniform vec2 u_scale;
varying vec2 v_uv; varying vec4 v_shape; varying vec2 v_rs; varying vec4 v_col;
void main() {
  gl_Position = vec4(a_pos * u_scale + vec2(-1.0, 1.0), 0.0, 1.0);
  v_uv = a_uv; v_shape = a_shape; v_rs = a_rs; v_col = a_col;
})";
constexpr const char* fragmentShader = R"(
precision mediump float;
uniform sampler2D u_atlas;
uniform sampler2D u_blit;
varying vec2 v_uv; varying vec4 v_shape; varying vec2 v_rs; varying vec4 v_col;
void main() {
  if (v_uv.x >= 0.0 && v_rs.y < 0.0) {
    vec4 px = texture2D(u_blit, v_uv).bgra;
    gl_FragColor = vec4(px.rgb * v_col.rgb, px.a * v_col.a);
    return;
  }
  float a;
  if (v_uv.x >= 0.0) {
    a = texture2D(u_atlas, v_uv).a;
  } else {
    vec2 q = abs(v_shape.xy) - (v_shape.zw - vec2(v_rs.x));
    float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - v_rs.x;
    a = v_rs.y > 0.0 ? clamp(0.5 - (abs(d) - 0.5 * v_rs.y), 0.0, 1.0) : clamp(0.5 - d, 0.0, 1.0);
  }
  gl_FragColor = vec4(v_col.rgb, v_col.a * a);
})";

double nowMs() noexcept
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

constexpr EGLint kBufferAgeExt = 0x313D; // EGL_BUFFER_AGE_EXT (EGL_EXT_buffer_age)

class GlesRenderer final : public Renderer
{
public:
    GlesRenderer(bool fb, const RendererEnv& env)
        : backend_(fb ? "gl-fb" : "gl-x11"), fb_(fb), env_(env), atlas_(*env.atlas), damage_(env.atlas)
    {
    }

    void setDamageTracking(bool on) override { damage_.setEnabled(on); }
    void invalidate() override { damage_.forceFull(); }

    bool init(std::string& error)
    {
        if (! env_.loadGl || ! env_.nativeWindow)
            return fail(error, "no GL on this platform");
        if (! env_.loadGl(fb_ ? "fb" : "x11", error))
            return false;
        auto native = env_.nativeWindow(fb_ ? "fb" : "x11", error);
        if (! native)
            return false;
        native_ = *native;
        void* nativeDisplay = native_.display;
        const auto nativeWindow = toNative<EGLNativeWindowType>(native_.window);
        const std::string& fbLine = native_.info;
        EGLint major = 0, minor = 0, count = 0;
        dpy_ = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(nativeDisplay));
        if (dpy_ == nullptr || ! eglInitialize(dpy_, &major, &minor))
            return failEgl(error, "eglInitialize");
        eglBindAPI(EGL_OPENGL_ES_API);
        const char* extensions = eglQueryString(dpy_, EGL_EXTENSIONS);
        bufferAge_ = extensions != nullptr && std::strstr(extensions, "EGL_EXT_buffer_age") != nullptr;
        EGLConfig config = nullptr;
        if (! bufferAge_) // a preserved back buffer lets damaged frames redraw only the damage
        {
            static const EGLint preservedAttrs[] = {EGL_SURFACE_TYPE,
                                                    EGL_WINDOW_BIT | EGL_SWAP_BEHAVIOR_PRESERVED_BIT,
                                                    EGL_RENDERABLE_TYPE,
                                                    EGL_OPENGL_ES2_BIT,
                                                    EGL_RED_SIZE,
                                                    5,
                                                    EGL_GREEN_SIZE,
                                                    6,
                                                    EGL_BLUE_SIZE,
                                                    5,
                                                    EGL_NONE};
            preserved_ = eglChooseConfig(dpy_, preservedAttrs, &config, 1, &count) && count >= 1;
        }
        if (! preserved_)
        {
            static const EGLint configAttrs[] = {EGL_SURFACE_TYPE,
                                                 EGL_WINDOW_BIT,
                                                 EGL_RENDERABLE_TYPE,
                                                 EGL_OPENGL_ES2_BIT,
                                                 EGL_RED_SIZE,
                                                 5,
                                                 EGL_GREEN_SIZE,
                                                 6,
                                                 EGL_BLUE_SIZE,
                                                 5,
                                                 EGL_NONE};
            if (! eglChooseConfig(dpy_, configAttrs, &config, 1, &count) || count < 1)
                return failEgl(error, "eglChooseConfig");
        }
        surface_ = eglCreateWindowSurface(dpy_, config, nativeWindow, nullptr);
        if (surface_ == nullptr)
            return failEgl(error, "eglCreateWindowSurface");
        if (preserved_ && ! eglSurfaceAttrib(dpy_, surface_, EGL_SWAP_BEHAVIOR, EGL_BUFFER_PRESERVED))
            preserved_ = false;
        static const EGLint contextAttrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
        context_ = eglCreateContext(dpy_, config, nullptr, contextAttrs);
        if (context_ == nullptr || ! eglMakeCurrent(dpy_, surface_, surface_, context_))
            return failEgl(error, "eglCreateContext/eglMakeCurrent");
        eglSwapInterval(dpy_, 1);
        eglQuerySurface(dpy_, surface_, EGL_WIDTH, &w_);
        eglQuerySurface(dpy_, surface_, EGL_HEIGHT, &h_);

        if (! buildProgram(error))
            return false;
        glActiveTexture(GL_TEXTURE0);
        glGenTextures(1, &atlasTex_);
        glBindTexture(GL_TEXTURE_2D, atlasTex_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, atlas_.width(), atlas_.height(), 0, GL_ALPHA, GL_UNSIGNED_BYTE,
                     atlas_.pixels());
        glActiveTexture(GL_TEXTURE1);
        glGenTextures(1, &blitTex_);
        glBindTexture(GL_TEXTURE_2D, blitTex_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        const std::uint32_t kOnePixel = 0xFFFFFFFFu;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &kOnePixel);
        glActiveTexture(GL_TEXTURE0);
        glViewport(0, 0, w_, h_);
        glDisable(GL_DITHER);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        verts_.reserve(16384);
        batches_.reserve(8);

        const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        const char* vendor = eglQueryString(dpy_, EGL_VENDOR);
        char info[400];
        std::snprintf(info, sizeof info, "%s surface %dx%d EGL %d.%d (%s) GL_RENDERER=%s GL_VERSION=%s %s",
                      backend_.c_str(), w_, h_, static_cast<int>(major), static_cast<int>(minor), vendor ? vendor : "?",
                      renderer ? renderer : "?", version ? version : "?", fbLine.c_str());
        info_ = std::string(info) + " damage=" +
                (bufferAge_   ? "buffer-age"
                 : preserved_ ? "preserved-swap"
                              : "full-redraw-on-change") +
                " | " + (env_.glReport ? env_.glReport() : std::string("?"));
        lastSwapMs_ = nowMs();
        return true;
    }

    std::string_view name() const noexcept override { return backend_; }
    int width() const noexcept override { return w_; }
    int height() const noexcept override { return h_; }
    std::string describe() const override { return info_; }

    bool render(const DrawList& list, FrameTiming& t) override
    {
        const double a = nowMs();
        const auto kind = damage_.compute(list, w_, h_);
        if (kind == FrameDamage::Kind::None)
        {
            t.damagedPixels = 0;
            t.rasterMs = nowMs() - a;
            const double wait = lastSwapMs_ + 1000.0 / 60.0 - nowMs(); // keep the vsynced loop at 60 Hz
            if (wait > 0)
                std::this_thread::sleep_for(std::chrono::microseconds(static_cast<long>(wait * 1000)));
            lastSwapMs_ = std::max(lastSwapMs_ + 1000.0 / 60.0, nowMs() - 1000.0 / 60.0);
            t.presentMs = 0;
            return true;
        }
        IRect now{};
        for (const auto& r : damage_.rects())
            now = unite(now, r);
        // What must be redrawn: this frame's damage plus what the back buffer missed since it was last drawn.
        EGLint age = 0;
        if (bufferAge_ && ! eglQuerySurface(dpy_, surface_, kBufferAgeExt, &age))
            age = 0;
        if (preserved_)
            age = 1;
        IRect region = now;
        if (kind == FrameDamage::Kind::Full || age <= 0 || age > static_cast<EGLint>(history_.size()))
            region = {0, 0, w_, h_};
        else
            for (EGLint k = 1; k < age; ++k)
                region = unite(region, history_[static_cast<std::size_t>(k - 1)]);
        for (std::size_t k = history_.size() - 1; k > 0; --k)
            history_[k] = history_[k - 1];
        history_[0] = now;
        const bool partial = region.area() < static_cast<long>(w_) * h_;
        region_ = region;
        t.damagedPixels = region.area();
        build(list);
        if (partial)
        {
            glEnable(GL_SCISSOR_TEST);
            glScissor(region.x0, h_ - region.y1, region.x1 - region.x0,
                      region.y1 - region.y0); // GL: origin bottom-left
        }
        else
            glDisable(GL_SCISSOR_TEST);
        glClear(GL_COLOR_BUFFER_BIT);
        if (! verts_.empty())
        {
            const auto* base = reinterpret_cast<const char*>(verts_.data());
            glVertexAttribPointer(0, 2, GL_FLOAT, 0, sizeof(Vertex), base + offsetof(Vertex, x));
            glVertexAttribPointer(1, 2, GL_FLOAT, 0, sizeof(Vertex), base + offsetof(Vertex, u));
            glVertexAttribPointer(2, 4, GL_FLOAT, 0, sizeof(Vertex), base + offsetof(Vertex, lx));
            glVertexAttribPointer(3, 2, GL_FLOAT, 0, sizeof(Vertex), base + offsetof(Vertex, radius));
            glVertexAttribPointer(4, 4, GL_UNSIGNED_BYTE, 1, sizeof(Vertex), base + offsetof(Vertex, rgba));
            for (const auto& b : batches_)
            {
                if (b.count <= 0)
                    continue;
                if (b.blit != nullptr)
                    uploadBlit(*b.blit);
                glDrawArrays(GL_TRIANGLES, b.first, b.count);
            }
        }
        const double b = nowMs();
        const bool ok = eglSwapBuffers(dpy_, surface_) != 0; // waits for vsync with swap interval 1
        lastSwapMs_ = nowMs();
        t.rasterMs = b - a;
        t.presentMs = lastSwapMs_ - b;
        if (! ok)
            damage_.forceFull();
        return ok;
    }

    bool readPixels(std::vector<std::uint32_t>& out) override
    {
        std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w_) * static_cast<std::size_t>(h_) * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, w_, h_, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        out.resize(static_cast<std::size_t>(w_) * static_cast<std::size_t>(h_));
        for (int y = 0; y < h_; ++y)
            for (int x = 0; x < w_; ++x)
            {
                const auto* p = &rgba[(static_cast<std::size_t>(h_ - 1 - y) * static_cast<std::size_t>(w_) +
                                       static_cast<std::size_t>(x)) *
                                      4];
                out[static_cast<std::size_t>(y) * static_cast<std::size_t>(w_) + static_cast<std::size_t>(x)] =
                    static_cast<std::uint32_t>(p[0]) << 16 | static_cast<std::uint32_t>(p[1]) << 8 | p[2];
            }
        return true;
    }

private:
    bool fail(std::string& error, const char* what)
    {
        error = backend_ + ": " + what;
        return false;
    }
    bool failEgl(std::string& error, const char* what)
    {
        char buf[96];
        std::snprintf(buf, sizeof buf, "%s failed (EGL error 0x%x)", what, static_cast<unsigned>(eglGetError()));
        return fail(error, buf);
    }

    GLuint shader(GLenum type, const char* src, std::string& error)
    {
        const GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (! ok)
        {
            char log[512] = "";
            glGetShaderInfoLog(s, sizeof log, nullptr, log);
            fail(error, (std::string("shader compile: ") + log).c_str());
            return 0;
        }
        return s;
    }

    bool buildProgram(std::string& error)
    {
        const GLuint vs = shader(GL_VERTEX_SHADER, vertexShader, error);
        const GLuint fs = vs ? shader(GL_FRAGMENT_SHADER, fragmentShader, error) : 0;
        if (fs == 0)
            return false;
        program_ = glCreateProgram();
        glAttachShader(program_, vs);
        glAttachShader(program_, fs);
        static const char* attribs[] = {"a_pos", "a_uv", "a_shape", "a_rs", "a_col"};
        for (GLuint i = 0; i < 5; ++i)
            glBindAttribLocation(program_, i, attribs[i]);
        glLinkProgram(program_);
        GLint linked = 0;
        glGetProgramiv(program_, GL_LINK_STATUS, &linked);
        if (! linked)
        {
            char log[512] = "";
            glGetProgramInfoLog(program_, sizeof log, nullptr, log);
            return fail(error, (std::string("shader link: ") + log).c_str());
        }
        glUseProgram(program_);
        glUniform2f(glGetUniformLocation(program_, "u_scale"), 2.0f / static_cast<float>(w_),
                    -2.0f / static_cast<float>(h_));
        glUniform1i(glGetUniformLocation(program_, "u_atlas"), 0);
        glUniform1i(glGetUniformLocation(program_, "u_blit"), 1);
        for (GLuint i = 0; i < 5; ++i)
            glEnableVertexAttribArray(i);
        glClearColor(0, 0, 0, 1);
        return true;
    }

    void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, float cx, float cy,
              float hw, float hh, float r, float sw, std::uint32_t argb)
    {
        if (x1 < static_cast<float>(region_.x0) || x0 > static_cast<float>(region_.x1) ||
            y1 < static_cast<float>(region_.y0) || y0 > static_cast<float>(region_.y1))
            return; // outside the scissor: nothing to draw
        const std::uint8_t c[4] = {static_cast<std::uint8_t>(argb >> 16), static_cast<std::uint8_t>(argb >> 8),
                                   static_cast<std::uint8_t>(argb), static_cast<std::uint8_t>(argb >> 24)};
        const float xs[6] = {x0, x1, x0, x0, x1, x1}, ys[6] = {y0, y0, y1, y1, y0, y1};
        const float us[6] = {u0, u1, u0, u0, u1, u1}, vs[6] = {v0, v0, v1, v1, v0, v1};
        for (int k = 0; k < 6; ++k)
        {
            Vertex v{xs[k], ys[k], us[k], vs[k], xs[k] - cx, ys[k] - cy, hw, hh, r, sw, {c[0], c[1], c[2], c[3]}, 0};
            verts_.push_back(v);
        }
    }

    struct Batch
    {
        GLint first = 0;
        GLsizei count = 0;
        const DrawCmd* blit = nullptr;
    };

    void uploadBlit(const DrawCmd& c)
    {
        if (c.pixels == nullptr || c.srcW == 0 || c.srcH == 0)
            return;
        if (c.pixels == lastBlitPixels_ && c.srcW == lastBlitW_ && c.srcH == lastBlitH_ &&
            c.textOffset == lastBlitSerial_)
            return;
        const std::uint32_t* src = c.pixels;
        if (c.srcStride != c.srcW)
        {
            blitScratch_.resize(static_cast<std::size_t>(c.srcW) * static_cast<std::size_t>(c.srcH));
            for (int y = 0; y < c.srcH; ++y)
                std::memcpy(blitScratch_.data() + static_cast<std::ptrdiff_t>(y) * c.srcW,
                            c.pixels + static_cast<std::ptrdiff_t>(y) * c.srcStride,
                            static_cast<std::size_t>(c.srcW) * sizeof(std::uint32_t));
            src = blitScratch_.data();
        }
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, blitTex_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        if (c.srcW != blitTexW_ || c.srcH != blitTexH_)
        {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, c.srcW, c.srcH, 0, GL_RGBA, GL_UNSIGNED_BYTE, src);
            blitTexW_ = c.srcW;
            blitTexH_ = c.srcH;
        }
        else
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, c.srcW, c.srcH, GL_RGBA, GL_UNSIGNED_BYTE, src);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glActiveTexture(GL_TEXTURE0);
        lastBlitPixels_ = c.pixels;
        lastBlitW_ = c.srcW;
        lastBlitH_ = c.srcH;
        lastBlitSerial_ = c.textOffset;
    }

    void build(const DrawList& list)
    {
        verts_.clear();
        batches_.clear();
        const DrawCmd* batchBlit = nullptr;
        GLint batchStart = 0;
        const auto flushBatch = [&]()
        {
            const auto count = static_cast<GLsizei>(verts_.size()) - batchStart;
            if (count > 0)
                batches_.push_back({batchStart, count, batchBlit});
            batchStart = static_cast<GLint>(verts_.size());
            batchBlit = nullptr;
        };
        const auto t = CanvasTransform::fit(w_, h_);
        const float aw = 1.0f / static_cast<float>(atlas_.width()), ah = 1.0f / static_cast<float>(atlas_.height());
        for (const auto& c : list.commands())
        {
            if (c.op == DrawCmd::Op::Blit)
            {
                if (batchBlit != nullptr &&
                    (batchBlit->pixels != c.pixels || batchBlit->srcW != c.srcW || batchBlit->srcH != c.srcH ||
                     batchBlit->textOffset != c.textOffset))
                    flushBatch();
                batchBlit = &c;
                const float x0 = t.offsetX + c.rect.x * t.scale, y0 = t.offsetY + c.rect.y * t.scale;
                const float x1 = x0 + c.rect.w * t.scale, y1 = y0 + c.rect.h * t.scale;
                quad(x0, y0, x1, y1, 0.0f, 0.0f, 1.0f, 1.0f, 0, 0, 0, 0, 0, -1.0f, c.argb);
                continue;
            }
            if (c.op == DrawCmd::Op::Text)
            {
                const auto s = list.textOf(c);
                const auto& m = atlas_.metrics(c.style);
                float pen = t.offsetX + c.rect.x * t.scale;
                if (c.align != Align::Left)
                {
                    const float w = atlas_.measure(c.style, s);
                    pen -= c.align == Align::Centre ? w * 0.5f : w;
                }
                const float baseline = t.offsetY + c.rect.y * t.scale + m.baselineBelowCentre();
                for (std::size_t i = 0; i < s.size();)
                {
                    const auto* g = atlas_.find(c.style, nextCodepoint(s, i));
                    if (g->w != 0)
                    {
                        const float gx = std::round(pen + g->left), gy = std::round(baseline + g->top);
                        quad(gx, gy, gx + g->w, gy + g->h, g->x * aw, g->y * ah, (g->x + g->w) * aw, (g->y + g->h) * ah,
                             0, 0, 0, 0, 0, 0, c.argb);
                    }
                    pen += g->advance;
                }
                continue;
            }
            const float x0 = t.offsetX + c.rect.x * t.scale, y0 = t.offsetY + c.rect.y * t.scale;
            const float hw = c.rect.w * t.scale * 0.5f, hh = c.rect.h * t.scale * 0.5f;
            const float cx = x0 + hw, cy = y0 + hh;
            const float sw = c.op == DrawCmd::Op::Stroke ? c.strokeWidth * t.scale : 0.0f;
            const float r = std::min(c.radius * t.scale, std::min(hw, hh));
            const float grow = sw * 0.5f + 1.0f; // room for the outline and the anti-aliased edge
            quad(x0 - grow, y0 - grow, x0 + 2 * hw + grow, y0 + 2 * hh + grow, -1, -1, -1, -1, cx, cy, hw, hh, r, sw,
                 c.argb);
        }
        flushBatch();
    }

    std::string backend_;
    bool fb_;
    RendererEnv env_;
    NativeWindow native_{};
    const GlyphAtlas& atlas_;
    EGLDisplay dpy_ = nullptr;
    EGLSurface surface_ = nullptr;
    EGLContext context_ = nullptr;
    EGLint w_ = 0, h_ = 0;
    GLuint program_ = 0, atlasTex_ = 0, blitTex_ = 0;
    int blitTexW_ = 1, blitTexH_ = 1;
    const std::uint32_t* lastBlitPixels_ = nullptr;
    std::uint16_t lastBlitW_ = 0, lastBlitH_ = 0;
    std::uint32_t lastBlitSerial_ = 0;
    std::vector<std::uint32_t> blitScratch_;
    std::vector<Vertex> verts_;
    std::vector<Batch> batches_;
    std::string info_;
    FrameDamage damage_;
    std::array<IRect, 4> history_{}; // union of the damage of the last frames, newest first (buffer age)
    IRect region_{};                 // what this frame redraws (scissor), surface pixels, y down
    bool bufferAge_ = false, preserved_ = false;
    double lastSwapMs_ = 0;
};

std::unique_ptr<Renderer> makeGles(bool fb, const RendererEnv& env, std::string& error)
{
    if (env.atlas == nullptr)
    {
        error = "gl needs the glyph atlas";
        return nullptr;
    }
    auto r = std::make_unique<GlesRenderer>(fb, env);
    if (! r->init(error))
        return nullptr;
    return r;
}

std::unique_ptr<Renderer> makeGlFb(const RendererEnv& env, std::string& error)
{
    return makeGles(true, env, error);
}
std::unique_ptr<Renderer> makeGlX11(const RendererEnv& env, std::string& error)
{
    return makeGles(false, env, error);
}
} // namespace
} // namespace ui

// Probed in a forked child first: they load a GL driver flavour.
APP_REGISTER_RENDERER("gl-fb", &ui::makeGlFb, true)
APP_REGISTER_RENDERER("gl-x11", &ui::makeGlX11, true)
