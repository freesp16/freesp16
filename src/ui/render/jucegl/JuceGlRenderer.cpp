// Backend (a) "juce-gl": JUCE 9's OpenGLContext (OpenGL ES 2 via EGL on an X11 window, JUCE_OPENGL_ES=1) attached to
// a borderless desktop Component; each frame the DrawList is replayed through JUCE's OpenGL Graphics renderer
// (createOpenGLGraphicsContext) on JUCE's GL thread. This is the "use JUCE for everything" option.
//
// EGL/GLES symbols JUCE calls are bound to the -x11 driver flavour by the platform's GL loader (RendererEnv::loadGl,
// gldispatch on imx6: load("x11") before attaching), which also bridges JUCE's EGL 1.5 calls on an EGL 1.4 driver.
// The backend initialises JUCE's GUI (ScopedJuceInitialiser_GUI) on first use; it stays up for the process.
//
// Damage: JUCE repaints the whole GL frame, so this backend only skips frames in which nothing changed (the frame diff
// of ui/render/Damage.h), sleeping to the next 60 Hz tick instead of swapping.
//
// Threading: render() runs on the app's main thread, which is JUCE's message thread; it pumps JUCE's dispatch loop
// (JUCE_MODAL_LOOPS_PERMITTED=1 on the device) while JUCE's GL thread renders, then waits for the frame.
#include "ui/render/GlyphAtlas.h"
#include "ui/render/JuceReplay.h"
#include "ui/render/RendererRegistry.h"

#include <juce_opengl/juce_opengl.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace ui
{
namespace
{
double nowMs() noexcept
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

class JuceGlRenderer final : public Renderer, private juce::OpenGLRenderer
{
public:
    explicit JuceGlRenderer(const RendererEnv& env)
        : env_(env), w_(env.width), h_(env.height),
          replay_(embedded(env) ? nullptr : env.fontData, embedded(env) ? 0 : env.fontSize), damage_(env.atlas)
    {
    }

    static bool embedded(const RendererEnv& env)
    {
        return env.fontData == nullptr || (env.atlas != nullptr && env.atlas->embeddedFonts());
    }
    void setDamageTracking(bool on) override { damage_.setEnabled(on); }
    void invalidate() override { damage_.forceFull(); }

    ~JuceGlRenderer() override
    {
        context_.detach();
        window_.reset();
    }

    bool init(std::string& error)
    {
        if (! env_.loadGl)
        {
            error = "juce-gl: no GL on this platform";
            return false;
        }
        if (! env_.loadGl("x11", error))
            return false;
        window_ = std::make_unique<juce::Component>();
        window_->setOpaque(true);
        window_->setBounds(0, 0, w_, h_);
        window_->addToDesktop(0); // style 0: no decorations (the device's X server has no window manager)
        window_->setVisible(true);
        context_.setRenderer(this);
        context_.setComponentPaintingEnabled(false); // only renderOpenGL draws
        context_.setContinuousRepainting(false);
        context_.setSwapInterval(1);
        context_.attachTo(*window_);
        const double deadline = nowMs() + 4000.0;
        while (! created_ && nowMs() < deadline)
            juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
        if (! created_)
        {
            error = "juce-gl: OpenGLContext was not created within 4 s (" + glReport() + ")";
            return false;
        }
        return true;
    }

    std::string_view name() const noexcept override { return "juce-gl"; }
    int width() const noexcept override { return w_; }
    int height() const noexcept override { return h_; }
    std::string describe() const override
    {
        return "juce-gl JUCE " + juce::SystemStats::getJUCEVersion().toStdString() + " " + glInfo_ + " | " + glReport();
    }
    bool readPixels(std::vector<std::uint32_t>&) override { return false; } // screenshots come from the X server

    bool render(const DrawList& list, FrameTiming& t) override
    {
        const double start = nowMs();
        if (damage_.compute(list, w_, h_) == FrameDamage::Kind::None)
        {
            t.damagedPixels = 0;
            t.rasterMs = nowMs() - start;
            const double wait = lastFrameMs_ + 1000.0 / 60.0 - nowMs();
            if (wait > 0)
                std::this_thread::sleep_for(std::chrono::microseconds(static_cast<long>(wait * 1000)));
            lastFrameMs_ = nowMs();
            t.presentMs = 0;
            return true;
        }
        t.damagedPixels = static_cast<long>(w_) * h_;
        pending_ = &list;
        done_.reset();
        context_.triggerRepaint();
        bool finished = false;
        while (! (finished = done_.wait(0)) && nowMs() - start < 250.0)
            juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
        pending_ = nullptr;
        t.rasterMs = rasterMs_.load();
        lastFrameMs_ = nowMs();
        t.presentMs = (lastFrameMs_ - start) - t.rasterMs;
        if (! finished)
            damage_.forceFull();
        return finished;
    }

private:
    void newOpenGLContextCreated() override
    {
        const auto* renderer = reinterpret_cast<const char*>(juce::gl::glGetString(juce::gl::GL_RENDERER));
        const auto* version = reinterpret_cast<const char*>(juce::gl::glGetString(juce::gl::GL_VERSION));
        glInfo_ =
            std::string("GL_RENDERER=") + (renderer ? renderer : "?") + " GL_VERSION=" + (version ? version : "?");
        created_ = true;
    }

    void renderOpenGL() override
    {
        const DrawList* list = pending_;
        if (list == nullptr)
            return;
        const double start = nowMs();
        const auto scale = static_cast<float>(context_.getRenderingScale());
        const int pw = juce::roundToInt(scale * static_cast<float>(w_)),
                  ph = juce::roundToInt(scale * static_cast<float>(h_));
        juce::OpenGLHelpers::clear(juce::Colours::black);
        if (auto g = juce::createOpenGLGraphicsContext(context_, pw, ph))
        {
            juce::Graphics graphics(*g);
            replay_.draw(graphics, *list, pw, ph);
        }
        rasterMs_ = nowMs() - start;
        done_.signal();
    }

    void openGLContextClosing() override {}
    std::string glReport() const { return env_.glReport ? env_.glReport() : std::string("?"); }

    RendererEnv env_;
    int w_, h_;
    JuceReplay replay_;
    juce::OpenGLContext context_;
    std::unique_ptr<juce::Component> window_;
    std::atomic<const DrawList*> pending_{nullptr};
    std::atomic<bool> created_{false};
    std::atomic<double> rasterMs_{0};
    juce::WaitableEvent done_{true};
    std::string glInfo_;
    FrameDamage damage_;
    double lastFrameMs_ = 0;
};

// JUCE's GUI (message manager, desktop) for this backend; created on the app's main thread, kept for the process.
void ensureJuceGui()
{
    static std::unique_ptr<juce::ScopedJuceInitialiser_GUI> gui;
    if (! gui)
        gui = std::make_unique<juce::ScopedJuceInitialiser_GUI>();
}

std::unique_ptr<Renderer> makeJuceGl(const RendererEnv& env, std::string& error)
{
    ensureJuceGui();
    auto r = std::make_unique<JuceGlRenderer>(env);
    if (! r->init(error))
        return nullptr;
    return r;
}
} // namespace
} // namespace ui

APP_REGISTER_RENDERER("juce-gl", &ui::makeJuceGl, true) // probed in a forked child (loads a GL driver flavour)
