// Host preview: the Shell in a resizable JUCE window, drawn by the `soft` rasteriser at the window's scale
// (CanvasTransform::fit, UID-101), 60 Hz, redrawn only when the shell reports damage (dirty()).
//
// Keyboard (03-ui/control-surface.md UI-120…122): the host platform's own `keyboard` ControlSource
// (platform/host/input/KeyboardSource, the UI-120 map) attached to the window as a juce::KeyListener — the same map
// the host app uses; its events are polled once per tick and dispatched to the shell. The view itself sees a key
// first (the listener sits on the window, the view's parent) and keeps only what the map leaves to a front end: the
// keys a popup interprets (↑ ↓ PgUp PgDn Home End Tab ⌫ ← → Return Esc for lists, grids and the text dialog) and typed
// text (UI-122, Shell::typeText). Mouse (UI-123): click = touch, drag = touch drag (a column or a body's drag target),
// wheel = detents over a column, else the selector.
#include "ProjectIdentity.h"
#include "core/params/ParamRegistry.h"
#include "platform/Platform.h"
#include "ui/render/GlyphAtlas.h"
#include "ui/render/Renderer.h"
#include "ui/shell/Layout.h"
#include "ui/shell/Shell.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdio>
#include <memory>

namespace
{
using Kind = io::ControlEvent::Kind;

class PreviewView final : public juce::Component, private juce::Timer
{
public:
    PreviewView()
    {
        setWantsKeyboardFocus(true);
        setSize(1280, 768);
        // The host platform's keyboard source (UI-120 map): no engine here, so pad hits only select / flash.
        std::string log;
        if (auto machine = platform::select("host", log))
            for (auto& source : machine->controlSources({nullptr}))
                if (source->id() == "keyboard")
                {
                    std::string info;
                    source->start(info);
                    keys_ = std::move(source);
                }
        startTimerHz(60);
    }
    ~PreviewView() override
    {
        if (keys_ != nullptr)
            keys_->stop();
    }
    // The window (the view's parent) owns the listener slot, so the view's own keyPressed sees each key first.
    juce::KeyListener* keyListener() const { return dynamic_cast<juce::KeyListener*>(keys_.get()); }

    // --selftest <png>: drives the window's key and mouse path (pad 5 key, Return → TRACK MENU, Esc → BACK, a click on
    // the MIXER button, Home, Up = knob 1 +1) through the keyboard source, prints what happened, writes the last frame
    // and quits.
    void selfTest(const juce::File& png)
    {
        ensureSurface();
        auto line = [this](const char* what)
        {
            std::printf("SELFTEST %-28s screen=%.*s track=%d volume-col=%s\n", what,
                        static_cast<int>(shell_->router().current().id.size()), shell_->router().current().id.data(),
                        shell_->state().selectedTrack, shell_->column(0).spec ? "bound" : "empty");
        };
        std::printf("SELFTEST keyboard source %s\n", keyListener() != nullptr ? "attached" : "MISSING");
        typeKey(juce::KeyPress('a'));
        line("key a (pad 5)");
        typeKey(juce::KeyPress(juce::KeyPress::returnKey));
        line("Return (push)");
        typeKey(juce::KeyPress(juce::KeyPress::escapeKey));
        line("Esc (BACK)");
        const auto mixer = ui::layout::menuButton(1);
        dispatchTouch({mixer.x + 20, mixer.y + 20}, ui::touch::kDown);
        dispatchTouch({mixer.x + 20, mixer.y + 20}, ui::touch::kUp);
        line("click MIXER");
        typeKey(juce::KeyPress(juce::KeyPress::homeKey));
        line("Home");
        typeKey(juce::KeyPress(juce::KeyPress::upKey));
        line("Up (knob 1 +1)");
        timerCallback();
        timerCallback();
        juce::FileOutputStream out(png);
        if (out.openedOk())
        {
            out.setPosition(0);
            out.truncate();
            juce::PNGImageFormat().writeImageToStream(image_, out);
        }
        std::printf("SELFTEST wrote %s\n", png.getFullPathName().toRawUTF8());
    }

private:
    double now() const { return juce::Time::getMillisecondCounterHiRes(); }

    // A key as the window delivers it: the view first, then the keyboard source; then the key is let go.
    void typeKey(const juce::KeyPress& k)
    {
        auto* keys = keyListener();
        if (! keyPressed(k) && keys != nullptr)
            keys->keyPressed(k, this);
        if (keys != nullptr)
            keys->keyStateChanged(false, this);
        pumpKeys();
    }

    // The keyboard source's queued events → the shell (stamped with this window's clock).
    void pumpKeys()
    {
        if (keys_ == nullptr || shell_ == nullptr)
            return;
        events_.clear();
        keys_->poll(events_);
        for (auto e : events_)
        {
            e.timeMs = now();
            shell_->dispatch(e);
            dirty_ = true;
        }
    }

    void ensureSurface()
    {
        const int w = std::max(1, getWidth()), h = std::max(1, getHeight());
        if (shell_ != nullptr && w == image_.getWidth() && h == image_.getHeight())
            return;
        const float scale = ui::CanvasTransform::fit(w, h).scale;
        std::string error;
        atlas_ = std::make_unique<ui::GlyphAtlas>();
        if (! atlas_->build(scale, error))
            std::fprintf(stderr, "preview: glyph atlas: %s\n", error.c_str());
        raster_ = ui::makeSoftRasterizer(*atlas_);
        pixels_.assign(static_cast<std::size_t>(w * h), 0);
        image_ = juce::Image(juce::Image::RGB, w, h, false);
        if (shell_ == nullptr)
        {
            ui::ShellConfig cfg{core::builtinParams(), [this](ui::TextStyleId s, std::string_view t)
                                { return atlas_->measure(s, t) / atlas_->scale(); }};
            cfg.log = [](const std::string& line) { std::printf("%s\n", line.c_str()); };
            shell_ = std::make_unique<ui::Shell>(std::move(cfg));
            auto& t1 = shell_->state().tracks[0];
            t1.name = "Test tone 440 Hz";
            t1.hasContent = true;
            dl_.reserve(4096, 32768);
        }
        dirty_ = true;
    }

    void timerCallback() override
    {
        ensureSurface();
        pumpKeys();
        const double t = now();
        if (! dirty_ && ! shell_->dirty(t))
            return;
        dirty_ = false;
        dl_.clear();
        shell_->frame(dl_, t);
        const int w = image_.getWidth(), h = image_.getHeight();
        raster_->draw(dl_, pixels_.data(), w, h, w);
        juce::Image::BitmapData data(image_, juce::Image::BitmapData::writeOnly);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                data.setPixelColour(x, y, juce::Colour(0xFF000000u | pixels_[static_cast<std::size_t>(y * w + x)]));
        repaint();
    }

    void paint(juce::Graphics& g) override { g.drawImageAt(image_, 0, 0); }
    void resized() override { ensureSurface(); }

    void send(Kind k, io::Control c, int v = 0)
    {
        if (shell_ == nullptr)
            return;
        shell_->dispatch({k, c, static_cast<std::int16_t>(v), io::kSourceKeyboard, now()});
        dirty_ = true;
    }

    // Window pixels → the logical 800 × 480 point a touch carries (UI-110).
    io::ControlEvent touchAt(juce::Point<float> p, std::int16_t phase) const
    {
        const auto t = ui::CanvasTransform::fit(getWidth(), getHeight());
        io::ControlEvent e{Kind::Touch, io::Control::Screen, phase, io::kSourceKeyboard, now()};
        e.x = static_cast<std::int16_t>((p.x - t.offsetX) / t.scale / ui::touch::kToCanvas);
        e.y = static_cast<std::int16_t>((p.y - t.offsetY) / t.scale / ui::touch::kToCanvas);
        return e;
    }
    void mouseDown(const juce::MouseEvent& e) override { dispatchTouch(e.position, ui::touch::kDown); }
    void mouseDrag(const juce::MouseEvent& e) override { dispatchTouch(e.position, ui::touch::kDrag); }
    void mouseUp(const juce::MouseEvent& e) override { dispatchTouch(e.position, ui::touch::kUp); }
    void dispatchTouch(juce::Point<float> p, std::int16_t phase)
    {
        if (shell_ != nullptr)
            shell_->dispatch(touchAt(p, phase));
        dirty_ = true;
    }
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        const int detents = w.deltaY > 0 ? 1 : (w.deltaY < 0 ? -1 : 0);
        if (detents == 0)
            return;
        const auto t = ui::CanvasTransform::fit(getWidth(), getHeight());
        const float cx = (e.position.x - t.offsetX) / t.scale, cy = (e.position.y - t.offsetY) / t.scale;
        if (cy >= ui::layout::stripY + ui::layout::tabH && cx >= ui::layout::columnX0)
        {
            const int n = static_cast<int>((cx - ui::layout::columnX0) / ui::layout::columnW);
            if (n >= 0 && n < 6)
                return send(Kind::Turn, io::knob(n + 1), detents);
        }
        send(Kind::Turn, io::Control::Selector, detents);
    }

    // SHIFT comes from the source's modifier tracking (KeyMap::modifiers); a modifier-only change is not a key event.
    void modifierKeysChanged(const juce::ModifierKeys&) override
    {
        if (auto* keys = keyListener())
            keys->keyStateChanged(false, this);
        pumpKeys();
    }

    bool textPopupOpen() const
    {
        auto* top = shell_ != nullptr ? shell_->topPopup() : nullptr;
        return top != nullptr && top->kind() == ui::PopupSpec::Kind::Dialog;
    }

    // Only what a popup interprets itself (UI-122; list keys of popups-and-feedback.md); everything else returns
    // false and reaches the keyboard source on the window.
    bool keyPressed(const juce::KeyPress& key) override
    {
        if (shell_ == nullptr || shell_->topPopup() == nullptr)
            return false;
        const int code = key.getKeyCode();
        const auto mods = key.getModifiers();
        dirty_ = true;
        static const std::pair<int, ui::TextKey> keys[] = {{juce::KeyPress::backspaceKey, ui::TextKey::Backspace},
                                                           {juce::KeyPress::returnKey, ui::TextKey::Return},
                                                           {juce::KeyPress::escapeKey, ui::TextKey::Escape},
                                                           {juce::KeyPress::upKey, ui::TextKey::Up},
                                                           {juce::KeyPress::downKey, ui::TextKey::Down},
                                                           {juce::KeyPress::pageUpKey, ui::TextKey::PageUp},
                                                           {juce::KeyPress::pageDownKey, ui::TextKey::PageDown},
                                                           {juce::KeyPress::homeKey, ui::TextKey::Home},
                                                           {juce::KeyPress::endKey, ui::TextKey::End},
                                                           {juce::KeyPress::tabKey, ui::TextKey::Tab}};
        if (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey)
        {
            const bool left = code == juce::KeyPress::leftKey;
            if (shell_->textKey(mods.isAltDown() ? (left ? ui::TextKey::WordLeft : ui::TextKey::WordRight)
                                                 : (left ? ui::TextKey::Left : ui::TextKey::Right)))
                return true;
        }
        for (const auto& [k, t] : keys)
            if (code == k && shell_->textKey(t))
                return true;
        const auto ch = key.getTextCharacter();
        if (ch >= 32 && ! mods.isCommandDown() && (textPopupOpen() || juce::CharacterFunctions::isLetter(ch)))
        {
            const auto utf8 = juce::String::charToString(ch).toStdString();
            if (shell_->typeText(utf8) || textPopupOpen())
                return true; // a text popup takes every printable key (UI-122)
        }
        return false;
    }

    std::unique_ptr<ui::GlyphAtlas> atlas_;
    std::unique_ptr<ui::CpuRasterizer> raster_;
    std::vector<std::uint32_t> pixels_;
    juce::Image image_;
    std::unique_ptr<ui::Shell> shell_;
    ui::DrawList dl_;
    std::unique_ptr<io::ControlSource> keys_;
    std::vector<io::ControlEvent> events_;
    bool dirty_ = true;
};

class PreviewWindow final : public juce::DocumentWindow
{
public:
    PreviewWindow()
        : DocumentWindow(juce::String(identity::displayName) + " preview", juce::Colours::black,
                         DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar(true);
        auto* view = new PreviewView();
        setContentOwned(view, true);
        if (auto* keys = view->keyListener()) // the platform's keyboard source on the window
            addKeyListener(keys);
        setResizable(true, true);
        setResizeLimits(960, 576, 2560, 1536); // minimum window 960 × 576 (UID-101)
        centreWithSize(1280, 768);
        setVisible(true);
        getContentComponent()->grabKeyboardFocus();
    }
    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
};

class PreviewApp final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return juce::String(identity::displayName) + " preview"; }
    const juce::String getApplicationVersion() override { return identity::version; }
    void initialise(const juce::String& args) override
    {
        window_ = std::make_unique<PreviewWindow>();
        if (args.contains("--selftest"))
        {
            const auto png = args.fromFirstOccurrenceOf("--selftest", false, false).trim().unquoted();
            dynamic_cast<PreviewView*>(window_->getContentComponent())
                ->selfTest(juce::File::getCurrentWorkingDirectory().getChildFile(png));
            quit();
        }
    }
    void shutdown() override { window_ = nullptr; }

private:
    std::unique_ptr<PreviewWindow> window_;
};
} // namespace

START_JUCE_APPLICATION(PreviewApp)
