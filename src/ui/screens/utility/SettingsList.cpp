#include "ui/screens/utility/SettingsList.h"

#include "ProjectIdentity.h"
#include "core/params/ParamRegistry.h"
#include "services/settings/Preferences.h"
#include "ui/descriptors/ScreenSpec.h"
#include "ui/descriptors/SettingsSpec.h"
#include "ui/screens/sequence/M2Session.h"
#include "ui/screens/utility/UtilityEnv.h"
#include "ui/shell/Layout.h"
#include "ui/shell/ParamAccess.h"
#include "ui/shell/Popup.h"
#include "ui/shell/ShellApi.h"
#include "ui/shell/UiState.h"
#include "ui/theme/Draw.h"
#include "ui/widgets/ListRow.h"
#include "ui/widgets/Scrollbar.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>

namespace ui::utility
{
namespace
{
namespace tk = ::theme::tokens;
namespace L = layout;
namespace st = services::settings;
using Kind = io::ControlEvent::Kind;
using Store = Binding::Store;

constexpr float kX = 16, kW = 1240, kTitleW = 1248, kListY = 200, kListBottom = 608, kRowH = 44, kHeaderH = 36;
constexpr float kValueX = 664, kDividerX = 640, kCellX = 648, kCellW = 600;
constexpr int kInlineMax = 8; // UID-878: more values than this open a ListPopup

bool startsWith(std::string_view s, std::string_view p) noexcept { return s.substr(0, p.size()) == p; }

// ---- bindings -------------------------------------------------------------------------------------------------------
Binding resolve(std::string_view binds, const core::ParamRegistry& params)
{
    Binding b;
    if (binds.empty())
        return b;
    if (const auto* f = st::findProjectField(binds))
    {
        b.store = Store::Project;
        b.field = f;
        b.spec = &f->spec;
        return b;
    }
    if (binds == "settings.syncSource")
    {
        b.store = Store::Sync;
        b.spec = params.find("midi.syncSource");
        if (b.spec == nullptr)
            b.store = Store::None;
        return b;
    }
    std::string_view base = binds;
    if (const auto hash = binds.find('#'); hash != std::string_view::npos)
    {
        b.scope = static_cast<std::uint16_t>(std::atoi(std::string(binds.substr(hash + 1)).c_str()));
        base = binds.substr(0, hash);
    }
    if (const auto* p = params.find(base))
    {
        b.store = Store::Param;
        b.spec = p;
        return b;
    }
    if (const auto* p = st::PrefRegistry::builtin().paramOf(base))
    {
        b.store = Store::Pref;
        b.spec = p;
        b.key = std::string(binds);
        return b;
    }
    return b; // None: shown disabled
}

SettingsRow valueRow(std::string label, std::string_view binds, const core::ParamRegistry& params)
{
    SettingsRow r;
    r.kind = SettingsRow::Kind::Value;
    r.label = std::move(label);
    r.bind = resolve(binds, params);
    return r;
}

SettingsRow header(std::string label)
{
    SettingsRow r;
    r.kind = SettingsRow::Kind::Header;
    r.label = std::move(label);
    return r;
}

void addRegistered(std::string_view page, std::vector<SettingsRow>& out, const core::ParamRegistry& params,
                   std::string_view onlySection = {})
{
    const auto& reg = SettingsRegistry::builtin();
    const auto* prefs = st::installed();
    for (const auto& s : reg.sections(page))
    {
        if (! onlySection.empty() && s.id != onlySection)
            continue;
        std::vector<SettingsRow> rows;
        for (const auto& spec : reg.rows(page, s.id))
        {
            if (! spec.featureFlag.empty() && (prefs == nullptr || ! st::FeatureFlags(*prefs).isOn(spec.featureFlag)))
                continue;
            if (spec.infoSource == "needs:keyboard" && utilityEnv().keyboard && ! utilityEnv().keyboard())
                continue; // KEYBOARD MAP on a platform without a keyboard (the hardware)
            SettingsRow r;
            r.label = std::string(spec.label);
            switch (spec.kind)
            {
                case SettingsRowSpec::Kind::Value:
                    r.kind = SettingsRow::Kind::Value;
                    r.bind = resolve(spec.binds, params);
                    if (spec.infoSource == "needs:sync.link")
                        r.needs = "sync.link";
                    break;
                case SettingsRowSpec::Kind::Action:
                    r.kind = SettingsRow::Kind::Action;
                    if (startsWith(spec.opens, "action:"))
                        r.action = std::string(spec.opens.substr(7));
                    else
                        r.opens = std::string(spec.opens);
                    if (! startsWith(spec.infoSource, "needs:")) // a condition, not a value to show
                        r.info = std::string(spec.infoSource);
                    break;
                case SettingsRowSpec::Kind::Info:
                    r.kind = SettingsRow::Kind::Info;
                    r.info = std::string(spec.infoSource);
                    break;
            }
            rows.push_back(std::move(r));
        }
        if (rows.empty())
            continue;
        if (! s.title.empty())
            out.push_back(header(std::string(s.title)));
        for (auto& r : rows)
            out.push_back(std::move(r));
    }
}

// ---- the remembered focus (UI-009, UID-879), per shell state and page ------------------------------------------------
std::map<std::pair<const void*, std::string>, int>& memory()
{
    static std::map<std::pair<const void*, std::string>, int> m;
    return m;
}

class SettingsBody;

// While a value is edited: takes push / BACK / the rotary / touches (the shell routes them to the top popup) and hands
// them to the list. Not modal: no scrim, nothing drawn (the list draws the inset cell). HOME / a screen change cancels.
struct Editor
{
    SettingsBody* body = nullptr;
};

class EditCapture final : public PopupWidget
{
public:
    explicit EditCapture(std::weak_ptr<Editor> e) : PopupWidget(PopupSpec::Kind::Modal), editor_(std::move(e)) {}
    bool modal() const override { return false; }
    RectF bounds() const noexcept override { return {kCellX, 0, kCellW, 0}; }
    void build(DrawList&, const UiContext&) const override {}
    bool handle(const io::ControlEvent& e, UiContext&) override;
    void cancel() override;

private:
    std::weak_ptr<Editor> editor_;
};

class SettingsBody final : public Widget
{
public:
    SettingsBody(const ScreenSpec& screen, const WidgetArgs& a)
        : shell_(a.shell), state_(a.state), page_(screen.id), title_(screen.title)
    {
        const auto& params = shell_ != nullptr ? shell_->params() : core::builtinParams();
        params_ = &params;
        rows_ = buildRows(page_, params);
        numberRows();
        syncOptions_ = makeSyncOptions();
        if (page_ == "utility")
            for (const auto& r : rows_)
                sections_ += r.kind == SettingsRow::Kind::Header ? 1 : 0;
        focus_ = next(-1, 1);
        if (auto it = memory().find({state_, page_}); it != memory().end() && focusable(it->second))
            focus_ = it->second; // BACK from a sub-page lands on the row that opened it (UID-879)
        if (focus_ < 0)
            focus_ = 0;
        scroll();
        refreshInfo();
        refreshDisk();
    }
    ~SettingsBody() override
    {
        *alive_ = false;
        if (editor_)
            editor_->body = nullptr;
        memory()[{state_, page_}] = focus_;
    }

    bool fullHeight() const { return state_ && state_->skin == theme::Skin::Classic && page_ == "utility"; }
    float listTop() const { return fullHeight() ? 229 : kListY; }
    float listBottom() const { return fullHeight() ? 724 : kListBottom; }
    RectF bounds() const noexcept override { return {kX, L::contentTop, kTitleW, listBottom() - L::contentTop}; }
    bool dirty(double) const override { return diskArrived_; }

    void build(DrawList& dl, const UiContext& ctx) const override
    {
        diskArrived_ = false;
        const auto& p = ctx.palette;
        // PanelTitle (project.md): the page name; right: "n sections" (UTILITY) or the page's note.
        const auto titleFill = p.classic ? p.accentWeak : p.surface2;
        const float titleY = fullHeight() ? 173 : 152;
        const float inset = fullHeight() ? 13 : 0;
        const float left = kX + inset, width = kTitleW - 2 * inset;
        dl.fill({left, titleY, width, 48}, titleFill, p.classic ? 1 : tk::radiusM);
        dl.fill({left, titleY + 40, width, 8}, titleFill);
        draw::hline(dl, left, left + width, titleY + 48, p.border);
        dl.text(left + 24, titleY + 24, Align::Left, p.classic ? TextStyleId::Value : TextStyleId::Title, p.text1, title_);
        if (page_ == "utility")
            note_.format("%d section%s", sections_, sections_ == 1 ? "" : "s");
        else
            note_.set(pageNote(page_));
        if (! fullHeight())
            dl.text(left + width - 24, titleY + 24, Align::Right, TextStyleId::Body, p.text2, note_.view());
        float y = listTop();
        int shown = 0;
        for (int i = first_; i < static_cast<int>(rows_.size()); ++i)
        {
            const auto& row = rows_[static_cast<std::size_t>(i)];
            const bool head = row.kind == SettingsRow::Kind::Header;
            const float h = head ? kHeaderH : kRowH;
            if (y + h > listBottom() + 0.5f)
                break;
            const RectF r {left, y, kW - 2 * inset, h};
            ListRowModel m;
            m.label = row.label;
            m.header = head;
            m.zebra = zebra_[static_cast<std::size_t>(i)]; // per section, so a row keeps its fill while scrolling
            m.focused = i == focus_;
            m.disabled = ! head && ! available(row);
            m.opens = row.kind == SettingsRow::Kind::Action && ! row.opens.empty();
            m.labelX = 40;
            m.valueX = kCellX + 8 - left; // the editing cell starts 8 lpx before this (x 648)
            const bool editingHere = editing_ && i == focus_;
            char* buf = text_[static_cast<std::size_t>(shown % kMaxShown)];
            int len = 0;
            if (! head)
                len = valueText(row, editingHere ? editValue_ : currentValue(row), buf, kTextN);
            const std::string_view value(buf, static_cast<std::size_t>(std::max(0, len)));
            if (editingHere)
            {
                m.editing = true;
                m.value = value;
            }
            ListRow::draw(dl, ctx, r, m);
            if (row.kind == SettingsRow::Kind::Value || row.kind == SettingsRow::Kind::Info)
                if (! editingHere)
                    draw::vline(dl, kDividerX, y + 8, y + h - 8, p.border);
            if (! editingHere && ! value.empty())
                dl.text(kValueX, y + h / 2, Align::Left, p.classic ? TextStyleId::Value : TextStyleId::Body,
                        m.disabled ? p.text3 : (m.focused ? p.text1 : p.text2), value);
            if (row.track > 0)
                dl.fill({kX + 24, y + 12, 4, 20}, theme::trackColour(p, row.track), 2);
            y += h;
            ++shown;
        }
        ScrollbarModel sm;
        sm.total = static_cast<float>(rows_.size());
        sm.visible = static_cast<float>(shown);
        sm.first = static_cast<float>(first_);
        Scrollbar::draw(dl, ctx, {1256 - inset, listTop() + 4, 4, listBottom() - listTop() - 8}, sm);
    }

    bool handle(const io::ControlEvent& e, UiContext&) override
    {
        if (shell_ == nullptr)
            return false;
        if (e.control == io::Control::Selector && e.kind == Kind::Turn)
        {
            if (state_ != nullptr && state_->shift)
                jumpSection(e.value);
            else
                moveFocus(e.value);
            return true;
        }
        if (e.control == io::Control::SelectorPush && e.kind == Kind::Press)
        {
            push();
            return true;
        }
        if (touch::isTouch(e) && e.value == touch::kDown)
        {
            const int i = rowAt(touch::y(e));
            if (i < 0 || ! focusable(i))
                return false;
            if (i == focus_)
                push(); // a tap on the focused row = push (on its value: enter editing)
            else
            {
                focus_ = i;
                scroll();
            }
            return true;
        }
        return false;
    }

    // ---- editing (EditCapture) ----
    void editTurn(int detents, bool fine)
    {
        if (! editing_)
            return;
        const auto& row = rows_[static_cast<std::size_t>(focus_)];
        editValue_ = stepValue(row, editValue_, detents, fine);
        if (row.bind.store != Store::Project)
            write(row, editValue_); // live preview (BACK restores)
    }
    void editCommit()
    {
        if (! editing_)
            return;
        editing_ = false;
        const auto& row = rows_[static_cast<std::size_t>(focus_)];
        if (row.bind.store == Store::Project)
            commitProject(row, editValue_);
        else
            write(row, editValue_);
        log("commit");
    }
    void editCancel()
    {
        if (! editing_)
            return;
        editing_ = false;
        const auto& row = rows_[static_cast<std::size_t>(focus_)];
        if (row.bind.store != Store::Project)
            write(row, savedValue_);
        log("cancel");
    }
    // A touch while editing: ‹ / › step, anywhere else commits. True = consumed.
    void editTouch(float x, float y)
    {
        const int i = rowAt(y);
        if (i == focus_ && x >= kCellX && x < kCellX + 40)
            editTurn(-1, false);
        else if (i == focus_ && x >= kCellX + kCellW - 40 && x < kCellX + kCellW)
            editTurn(1, false);
        else
            editCommit();
    }
    bool editing() const noexcept { return editing_; }

    int focus() const noexcept { return focus_; }
    const std::vector<SettingsRow>& rows() const noexcept { return rows_; }
    std::string valueOf(int i) const
    {
        if (i < 0 || i >= static_cast<int>(rows_.size()))
            return {};
        char buf[kTextN];
        const auto& row = rows_[static_cast<std::size_t>(i)];
        const int n = valueText(row, editing_ && i == focus_ ? editValue_ : currentValue(row), buf, kTextN);
        return std::string(buf, static_cast<std::size_t>(std::max(0, n)));
    }

private:
    static constexpr int kMaxShown = 12, kTextN = 96;

    // ---- values ----
    st::Preferences* prefs() const noexcept { return st::installed(); }
    const core::model::Project& project() const { return m2::Session::instance().project(); }

    const std::vector<std::string>& syncOptions() const noexcept { return syncOptions_; }
    std::vector<std::string> makeSyncOptions() const
    {
        std::vector<std::string> out {"INTERNAL"};
        std::vector<std::string> ports;
        if (const auto& f = utilityEnv().midiPorts)
            ports = f();
        if (ports.empty())
            out.emplace_back("MIDI CLOCK");
        for (const auto& name : ports)
            out.push_back("MIDI CLOCK (" + name + ")");
        return out; // LINK (SYN-014) is P2: not offered until Ableton Link exists (our choice)
    }

    bool available(const SettingsRow& row) const
    {
        switch (row.kind)
        {
            case SettingsRow::Kind::Header: return false;
            case SettingsRow::Kind::Info: return true;
            case SettingsRow::Kind::Action:
                if (! row.opens.empty())
                    return ScreenRegistry::builtin().find(row.opens) != nullptr;
                return ! row.action.empty();
            case SettingsRow::Kind::Value:
                if (row.bind.store == Store::None || row.bind.spec == nullptr)
                    return false;
                if (row.needs == "sync.link")
                    return false; // SYNC SOURCE has no LINK option yet (SYN-014, P2)
                return true;
        }
        return false;
    }
    bool focusable(int i) const
    {
        if (i < 0 || i >= static_cast<int>(rows_.size()))
            return false;
        return available(rows_[static_cast<std::size_t>(i)]);
    }

    float currentValue(const SettingsRow& row) const
    {
        const auto& b = row.bind;
        switch (b.store)
        {
            case Store::None: return 0;
            case Store::Param:
                // A per-port preference (midi.portMode#p): the shell keeps one global value per ParamSpec, the
                // preferences one per port.
                if (b.scope != 0 && prefs() != nullptr && st::persists(*b.spec))
                    return prefs()->get(st::keyOf(*b.spec, b.scope));
                return shell_ != nullptr ? shell_->values().get(*b.spec, b.scope) : b.spec->defaultValue;
            case Store::Pref: return prefs() != nullptr ? prefs()->get(b.key) : b.spec->defaultValue;
            case Store::Project: return st::projectValue(project(), *b.field, b.scope);
            case Store::Sync:
            {
                if (shell_ == nullptr)
                    return 0;
                if (shell_->values().get(*b.spec, 0) < 0.5f)
                    return 0;
                const auto* port = params_->find("midi.syncPort");
                const int n = static_cast<int>(syncOptions().size()) - 1;
                const int pi = port != nullptr ? static_cast<int>(std::lround(shell_->values().get(*port, 0))) : 1;
                return static_cast<float>(std::clamp(pi, 1, std::max(1, n)));
            }
        }
        return 0;
    }

    int optionCount(const SettingsRow& row) const
    {
        const auto& b = row.bind;
        if (b.store == Store::Sync)
            return static_cast<int>(syncOptions().size());
        if (b.spec == nullptr)
            return 0;
        const bool list = b.spec->type == core::ParamType::Enum || b.spec->type == core::ParamType::Bool ||
                          (b.store == Store::Project && b.field->count > 1) || b.spec->id == "settings.pitchRange";
        return list ? static_cast<int>(std::lround(b.spec->maxValue - b.spec->minValue)) + 1 : 0;
    }
    float optionValue(const SettingsRow& row, int option) const
    {
        return row.bind.store == Store::Sync ? static_cast<float>(option) : row.bind.spec->minValue + static_cast<float>(option);
    }

    float stepValue(const SettingsRow& row, float v, int detents, bool fine) const
    {
        const auto& b = row.bind;
        if (b.store == Store::Sync)
            return static_cast<float>(std::clamp(static_cast<int>(std::lround(v)) + detents, 0, optionCount(row) - 1));
        if (b.store == Store::Project && b.field->id == "settings.pitchRange" && v < 0)
            v = 1; // an imported asymmetric range: the first turn lands on ±2
        return b.spec->step(v, detents, fine);
    }

    int valueText(const SettingsRow& row, float v, char* buf, int n) const
    {
        buf[0] = 0;
        if (row.kind == SettingsRow::Kind::Info || row.kind == SettingsRow::Kind::Action)
        {
            // The texts of the providers, read on entry (refreshInfo): no allocation while drawing.
            const auto i = static_cast<std::size_t>(&row - rows_.data());
            const auto& t = i < info_.size() ? info_[i] : std::string();
            return std::snprintf(buf, static_cast<std::size_t>(n), "%s", t.c_str());
        }
        if (row.kind != SettingsRow::Kind::Value)
            return 0;
        const auto& b = row.bind;
        if (b.store == Store::None || b.spec == nullptr)
            return std::snprintf(buf, static_cast<std::size_t>(n), "\xE2\x80\x94");
        if (b.store == Store::Project)
            return st::formatProjectValue(project(), *b.field, b.scope, v, buf, n);
        if (b.store == Store::Sync)
        {
            const auto& options = syncOptions();
            const auto i = static_cast<std::size_t>(std::clamp(static_cast<int>(std::lround(v)), 0,
                                                               static_cast<int>(options.size()) - 1));
            return std::snprintf(buf, static_cast<std::size_t>(n), "%s", options[i].c_str());
        }
        int len = b.spec->formatValue(v, buf, n);
        if (len < 0)
            len = 0;
        if (! b.spec->unit.empty() && len > 0 && len + 1 + static_cast<int>(b.spec->unit.size()) < n)
            len += std::snprintf(buf + len, static_cast<std::size_t>(n - len), " %.*s",
                                 static_cast<int>(b.spec->unit.size()), b.spec->unit.data());
        return len;
    }

    int infoText(const std::string& source, char* buf, int n) const
    {
        const auto size = static_cast<std::size_t>(n);
        const auto& env = utilityEnv();
        auto put = [&](const std::string& s) { return std::snprintf(buf, size, "%s", s.c_str()); };
        if (source.empty())
            return 0;
        if (source[0] == '=')
            return put(source.substr(1));
        if (source == "version")
            return put(env.version ? env.version() : std::string(identity::version));
        if (source == "diskSpace")
            return put(formatBytes(disk_));
        if (source == "audio.device")
            return put(env.audioDevice ? env.audioDevice() : std::string("\xE2\x80\x94"));
        if (source == "audio.format")
            return put(env.audioFormat ? env.audioFormat() : std::string("\xE2\x80\x94"));
        if (source == "audio.inputs")
            return put(env.inputs ? env.inputs() : std::string("\xE2\x80\x94"));
        if (source == "controllers")
        {
            const int c = env.controllers ? env.controllers() : -1;
            return c < 0 ? put("\xE2\x80\x94") : std::snprintf(buf, size, "%d connected", c);
        }
        if (source == "library.count")
        {
            const auto l = env.libraryLocations ? env.libraryLocations() : std::vector<LibraryLocation> {};
            return std::snprintf(buf, size, "%zu location%s", l.size(), l.size() == 1 ? "" : "s");
        }
        if (source == "midi.count")
        {
            const auto l = env.midiPorts ? env.midiPorts() : std::vector<std::string> {};
            return std::snprintf(buf, size, "%zu port%s", l.size(), l.size() == 1 ? "" : "s");
        }
        return 0;
    }

    void write(const SettingsRow& row, float v)
    {
        const auto& b = row.bind;
        switch (b.store)
        {
            case Store::None:
            case Store::Project: return;
            case Store::Param:
                shell_->values().set(*b.spec, b.scope, b.spec->clamp(v));
                if (prefs() != nullptr && st::persists(*b.spec)) // the app's PreferenceAccess does it too
                    prefs()->set(st::keyOf(*b.spec, b.scope), b.spec->clamp(v));
                if (b.spec->id == "ui.theme" && state_ != nullptr)
                {
                    state_->skin = theme::skinFromPreference(v);
                    scroll();
                }
                return;
            case Store::Pref:
                if (prefs() != nullptr)
                    prefs()->set(b.key, v);
                return;
            case Store::Sync:
            {
                const int option = static_cast<int>(std::lround(v));
                shell_->values().set(*b.spec, 0, option > 0 ? 1.0f : 0.0f);
                if (prefs() != nullptr)
                    prefs()->set(st::keyOf(*b.spec, 0), option > 0 ? 1.0f : 0.0f);
                if (const auto* port = params_->find("midi.syncPort"); port != nullptr && option > 0)
                {
                    shell_->values().set(*port, 0, static_cast<float>(option));
                    if (prefs() != nullptr)
                        prefs()->set(st::keyOf(*port, 0), static_cast<float>(option));
                }
                return;
            }
        }
    }
    void commitProject(const SettingsRow& row, float v)
    {
        auto& s = m2::Session::instance();
        if (auto cmd = st::setProjectValue(s.project(), *row.bind.field, row.bind.scope, v))
            s.execute(std::move(cmd));
    }

    // ---- navigation ----
    int next(int from, int dir) const
    {
        for (int i = from + dir; i >= 0 && i < static_cast<int>(rows_.size()); i += dir)
            if (focusable(i))
                return i;
        return -1;
    }
    void moveFocus(int detents)
    {
        const int dir = detents > 0 ? 1 : -1;
        for (int k = 0; k < std::abs(detents); ++k)
        {
            const int n = next(focus_, dir);
            if (n < 0)
                break;
            focus_ = n;
        }
        scroll();
    }
    void jumpSection(int detents)
    {
        // The first focusable row of the next (previous) section.
        const int dir = detents > 0 ? 1 : -1;
        for (int k = 0; k < std::abs(detents); ++k)
        {
            int h = focus_;
            if (dir < 0)
            {
                // the header of the focused row's section, then the one before it
                while (h >= 0 && rows_[static_cast<std::size_t>(h)].kind != SettingsRow::Kind::Header)
                    --h;
                if (next(h, 1) != focus_ && h >= 0)
                {
                    focus_ = next(h, 1);
                    continue;
                }
                --h;
                while (h >= 0 && rows_[static_cast<std::size_t>(h)].kind != SettingsRow::Kind::Header)
                    --h;
                const int n = next(std::max(-1, h), 1);
                if (n >= 0)
                    focus_ = n;
            }
            else
            {
                ++h;
                while (h < static_cast<int>(rows_.size()) && rows_[static_cast<std::size_t>(h)].kind != SettingsRow::Kind::Header)
                    ++h;
                const int n = next(h, 1);
                if (n >= 0)
                    focus_ = n;
            }
        }
        scroll();
    }
    void numberRows()
    {
        zebra_.assign(rows_.size(), 0);
        int z = 0;
        for (std::size_t i = 0; i < rows_.size(); ++i)
            z = rows_[i].kind == SettingsRow::Kind::Header ? 0 : (zebra_[i] = z, z + 1);
    }
    float heightOf(int i) const
    {
        return rows_[static_cast<std::size_t>(i)].kind == SettingsRow::Kind::Header ? kHeaderH : kRowH;
    }
    // Keeps the focused row (and its section header, when it fits) in view.
    void scroll()
    {
        if (rows_.empty())
            return;
        if (focus_ < first_)
            first_ = focus_;
        if (first_ > 0 && first_ == focus_ && rows_[static_cast<std::size_t>(first_ - 1)].kind == SettingsRow::Kind::Header)
            --first_;
        for (;;)
        {
            float y = listTop();
            for (int i = first_; i <= focus_; ++i)
                y += heightOf(i);
            if (y <= listBottom() + 0.5f || first_ >= focus_)
                break;
            ++first_;
        }
    }
    int rowAt(float y) const
    {
        float top = listTop();
        for (int i = first_; i < static_cast<int>(rows_.size()); ++i)
        {
            const float h = heightOf(i);
            if (top + h > listBottom() + 0.5f)
                break;
            if (y >= top && y < top + h)
                return i;
            top += h;
        }
        return -1;
    }

    void push();
    void runAction(const SettingsRow& row);
    void refreshDisk()
    {
        if (page_ != "utility" || ! utilityEnv().diskSpace)
            return;
        auto alive = alive_;
        utilityEnv().diskSpace([this, alive](double bytes) {
            if (! *alive)
                return;
            disk_ = bytes;
            refreshInfo();
            diskArrived_ = true;
        });
    }
    void refreshInfo()
    {
        info_.assign(rows_.size(), std::string());
        char buf[kTextN];
        for (std::size_t i = 0; i < rows_.size(); ++i)
            if (rows_[i].kind == SettingsRow::Kind::Info || rows_[i].kind == SettingsRow::Kind::Action)
            {
                const int len = infoText(rows_[i].info, buf, kTextN);
                info_[i].assign(buf, static_cast<std::size_t>(std::max(0, len)));
            }
    }
    void log(const char* what) const
    {
        (void)what; // the shell logs NAV / INPUT; the value lands in the preferences or the model
    }

    ShellApi* shell_;
    UiState* state_;
    const core::ParamRegistry* params_ = nullptr;
    std::string page_, title_;
    std::vector<SettingsRow> rows_;
    std::vector<int> zebra_; // the zebra index of each row within its section
    std::vector<std::string> info_, syncOptions_; // provider texts (per row) and SYNC SOURCE values, read on entry
    int sections_ = 0, focus_ = -1, first_ = 0;
    bool editing_ = false;
    float editValue_ = 0, savedValue_ = 0;
    double disk_ = -1;
    mutable bool diskArrived_ = false;
    std::shared_ptr<Editor> editor_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    mutable char text_[kMaxShown][kTextN] {};
    mutable FixedText<48> note_;
};

bool EditCapture::handle(const io::ControlEvent& e, UiContext&)
{
    auto ed = editor_.lock();
    SettingsBody* body = ed ? ed->body : nullptr;
    if (body == nullptr || ! body->editing())
    {
        closed = true;
        return false;
    }
    if (e.control == io::Control::Selector && e.kind == Kind::Turn)
    {
        body->editTurn(e.value, state != nullptr && state->shift);
        return true;
    }
    if (e.control == io::Control::SelectorPush && e.kind == Kind::Press)
    {
        body->editCommit();
        closed = true;
        return true;
    }
    if (e.control == io::Control::Back && e.kind == Kind::Press)
    {
        body->editCancel();
        closed = true;
        return true;
    }
    if (touch::isTouch(e) && e.value == touch::kDown)
    {
        body->editTouch(touch::x(e), touch::y(e));
        closed = ! body->editing();
        return true;
    }
    return false;
}

void EditCapture::cancel()
{
    if (auto ed = editor_.lock(); ed && ed->body != nullptr)
        ed->body->editCancel();
    closed = true;
}

void SettingsBody::push()
{
    if (focus_ < 0 || focus_ >= static_cast<int>(rows_.size()) || ! focusable(focus_))
        return;
    const auto& row = rows_[static_cast<std::size_t>(focus_)];
    switch (row.kind)
    {
        case SettingsRow::Kind::Header:
        case SettingsRow::Kind::Info: return; // info rows: push does nothing (utility.md)
        case SettingsRow::Kind::Action:
            if (! row.opens.empty())
            {
                memory()[{state_, page_}] = focus_;
                shell_->open(row.opens);
            }
            else
                runAction(row);
            return;
        case SettingsRow::Kind::Value: break;
    }
    const int options = optionCount(row);
    if (options > kInlineMax) // UID-878: a long list opens a ListPopup
    {
        ListRequest req;
        req.title = rows_[static_cast<std::size_t>(focus_)].label;
        const float cur = currentValue(row);
        char buf[kTextN];
        // The labels live as long as the popup (its items are views): owned by the onChoose closure.
        auto labels = std::make_shared<std::vector<std::string>>();
        labels->reserve(static_cast<std::size_t>(options));
        for (int o = 0; o < options; ++o)
        {
            const int n = valueText(row, optionValue(row, o), buf, kTextN);
            labels->emplace_back(buf, static_cast<std::size_t>(std::max(0, n)));
        }
        for (int o = 0; o < options; ++o)
        {
            req.items.push_back({(*labels)[static_cast<std::size_t>(o)], {}, {}, o});
            if (std::lround(optionValue(row, o)) == std::lround(cur))
                req.current = o;
        }
        const int at = focus_;
        auto alive = alive_;
        req.onChoose = [this, at, alive, labels](int, const ListItem& it) {
            if (! *alive)
                return;
            const auto& r = rows_[static_cast<std::size_t>(at)];
            const float v = optionValue(r, it.value);
            if (r.bind.store == Store::Project)
                commitProject(r, v);
            else
                write(r, v);
        };
        shell_->showList(std::move(req));
        return;
    }
    // Inline editing: the value cell becomes the inset box (UID-877).
    savedValue_ = editValue_ = currentValue(row);
    editing_ = true;
    if (! editor_)
    {
        editor_ = std::make_shared<Editor>();
        editor_->body = this;
    }
    if (shell_->showPopup(std::make_unique<EditCapture>(editor_)) == nullptr)
        editing_ = false; // the popup depth refused it
}

void SettingsBody::runAction(const SettingsRow& row)
{
    if (row.action == "reset.midiIn" || row.action == "reset.midiOut")
    {
        const bool in = row.action == "reset.midiIn";
        MessageRequest r;
        r.icon = glyph::Kind::Caution;
        r.title = "CHANNEL MAPPING RESET";
        r.lines = {in ? "Reset the MIDI IN channel mapping? Track n will receive on channel n."
                      : "Reset the MIDI OUT channel mapping? Track n will send on channel n."};
        r.buttons = {{"CANCEL", ButtonStyle::Plain}, {"RESET", ButtonStyle::Primary}};
        r.defaultButton = 0;
        r.onResult = [in](int button) {
            if (button != 1)
                return;
            auto& s = m2::Session::instance();
            if (auto cmd = st::resetChannelMapping(s.project(), in))
                s.execute(std::move(cmd));
        };
        shell_->showMessage(std::move(r));
        return;
    }
    if (row.action == "library.add")
    {
        DialogRequest d;
        d.title = "ADD LOCATION";
        d.prompt = "Folder to add to the library";
        d.hint = "/media/usb/Samples";
        d.buttons = {{"CANCEL", ButtonStyle::Plain}, {"ADD", ButtonStyle::Primary}};
        d.validate = [](std::string_view text) -> std::string {
            if (text.empty())
                return "Type a folder";
            return juce::File::isAbsolutePath(juce::String(std::string(text))) &&
                           juce::File(juce::String(std::string(text))).isDirectory()
                       ? std::string()
                       : std::string("No such folder");
        };
        auto alive = alive_;
        d.onResult = [this, alive](int button, std::string_view text) {
            if (button != 1 || prefs() == nullptr)
                return;
            auto list = prefs()->getList("library.locations");
            const std::string path(text);
            if (std::find(list.begin(), list.end(), path) == list.end())
                list.push_back(path);
            prefs()->setList("library.locations", list);
            if (utilityEnv().libraryChanged)
                utilityEnv().libraryChanged();
            if (*alive)
            {
                rows_ = buildRows(page_, *params_);
                numberRows();
                refreshInfo();
            }
        };
        shell_->showDialog(std::move(d));
        return;
    }
    if (startsWith(row.action, "library.remove:"))
    {
        const std::string path = row.action.substr(15);
        MessageRequest r;
        r.icon = glyph::Kind::Caution;
        r.title = "REMOVE LOCATION";
        r.lines = {"Remove " + path + " from the library locations?", "The files stay where they are."};
        r.buttons = {{"CANCEL", ButtonStyle::Plain}, {"REMOVE", ButtonStyle::Destructive}};
        r.defaultButton = 0;
        auto alive = alive_;
        r.onResult = [this, alive, path](int button) {
            if (button != 1 || prefs() == nullptr)
                return;
            auto list = prefs()->getList("library.locations");
            list.erase(std::remove(list.begin(), list.end(), path), list.end());
            prefs()->setList("library.locations", list);
            if (utilityEnv().libraryChanged)
                utilityEnv().libraryChanged();
            if (*alive)
            {
                rows_ = buildRows(page_, *params_);
                numberRows();
                refreshInfo();
                focus_ = std::min(focus_, static_cast<int>(rows_.size()) - 1);
                if (! focusable(focus_))
                    focus_ = next(-1, 1);
                scroll();
            }
        };
        shell_->showMessage(std::move(r));
    }
}

std::unique_ptr<Widget> makeSettingsList(const ScreenSpec& s, const WidgetArgs& a)
{
    return std::make_unique<SettingsBody>(s, a);
}
} // namespace

// ---- rows of a page -------------------------------------------------------------------------------------------------
std::vector<SettingsRow> buildRows(std::string_view page, const core::ParamRegistry& params)
{
    std::vector<SettingsRow> out;
    if (page == "utility.channelMapIn" || page == "utility.channelMapOut")
    {
        const bool in = page == "utility.channelMapIn";
        for (int t = 0; t < 16; ++t)
        {
            auto r = valueRow("TRACK " + std::to_string(t + 1), in ? "midi.in.channel" : "midi.out.channel", params);
            r.bind.scope = static_cast<std::uint16_t>(t);
            r.track = t + 1;
            out.push_back(std::move(r));
        }
        return out;
    }
    if (page == "utility.midiPorts")
    {
        addRegistered(page, out, params);
        std::vector<std::string> ports;
        if (const auto& f = utilityEnv().midiPorts)
            ports = f();
        if (ports.empty())
        {
            SettingsRow r;
            r.kind = SettingsRow::Kind::Info;
            r.label = "NO MIDI PORT";
            r.info = "=connect a MIDI device";
            out.push_back(header("PORTS"));
            out.push_back(std::move(r));
        }
        for (std::size_t p = 0; p < ports.size() && p < 8; ++p)
        {
            const auto n = std::to_string(p);
            out.push_back(header(ports[p]));
            out.push_back(valueRow("IN", "midi.portIn#" + n, params));
            out.push_back(valueRow("OUT MODE", "midi.portMode#" + n, params));
            out.push_back(valueRow("CLOCK OFFSET", "midi.portOffset#" + n, params));
        }
        return out;
    }
    if (page == "utility.library")
    {
        std::vector<LibraryLocation> locations;
        if (const auto& f = utilityEnv().libraryLocations)
            locations = f();
        int n = 0;
        for (const auto& l : locations)
        {
            SettingsRow r;
            r.label = n == 0 ? std::string("LIBRARY ROOT") : (l.label.empty() ? "LOCATION " + std::to_string(n) : l.label);
            r.info = "=" + l.path;
            if (l.removable)
            {
                r.kind = SettingsRow::Kind::Action;
                r.action = "library.remove:" + l.path;
            }
            else
                r.kind = SettingsRow::Kind::Info;
            out.push_back(std::move(r));
            ++n;
        }
        if (locations.empty())
        {
            SettingsRow r;
            r.kind = SettingsRow::Kind::Info;
            r.label = "LIBRARY ROOT";
            r.info = "=\xE2\x80\x94";
            out.push_back(std::move(r));
        }
        // Extra locations of the preference list that the provider did not list (not mounted now).
        if (const auto* prefs = st::installed())
            for (const auto& path : prefs->getList("library.locations"))
                if (std::none_of(locations.begin(), locations.end(), [&](const LibraryLocation& l) { return l.path == path; }))
                {
                    SettingsRow r;
                    r.kind = SettingsRow::Kind::Action;
                    r.label = "LOCATION " + std::to_string(n++);
                    r.info = "=" + path;
                    r.action = "library.remove:" + path;
                    out.push_back(std::move(r));
                }
        SettingsRow add;
        add.kind = SettingsRow::Kind::Action;
        add.label = "ADD LOCATION\xE2\x80\xA6";
        add.action = "library.add";
        out.push_back(std::move(add));
        return out;
    }
    addRegistered(page, out, params);
    return out;
}

std::string_view pageNote(std::string_view page)
{
    if (page == "utility.channelMapIn")
        return "track \xE2\x86\x90 receive channel";
    if (page == "utility.channelMapOut")
        return "track \xE2\x86\x92 send channel";
    if (page == "utility.outputMap")
        return "interface channel pairs";
    return {};
}

// ---- test access ----------------------------------------------------------------------------------------------------
namespace
{
const SettingsBody* asBody(const Widget* w) noexcept { return dynamic_cast<const SettingsBody*>(w); }
} // namespace
int settingsFocus(const Widget* body) noexcept
{
    const auto* b = asBody(body);
    return b != nullptr ? b->focus() : -1;
}
bool settingsEditing(const Widget* body) noexcept
{
    const auto* b = asBody(body);
    return b != nullptr && b->editing();
}
const std::vector<SettingsRow>* settingsRows(const Widget* body) noexcept
{
    const auto* b = asBody(body);
    return b != nullptr ? &b->rows() : nullptr;
}
std::string settingsValueText(const Widget* body, int row)
{
    const auto* b = asBody(body);
    return b != nullptr ? b->valueOf(row) : std::string {};
}
void forgetUtilityFocus() noexcept { memory().clear(); }
} // namespace ui::utility

APP_REGISTER_WIDGET("SettingsList", &ui::utility::makeSettingsList)
