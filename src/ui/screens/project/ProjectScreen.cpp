// PROJECT, OPEN PROJECT, DELETE PROJECT, the name dialogs and RENDER AUDIO (07-ui-design/screens/project.md;
// UI-316…319, UID-850…862). PROJECT is a full-width list under a title band (the open project's name and `· edited`
// while dirty, UID-862) in the order CREATE NEW, OPEN, SAVE, SAVE AS, DELETE, SAVE AS PACKAGE, RENDER AUDIO, then the
// Sixteen rows EXPORT SCENE, IMPORT SCENE (UID-850); slot A shows BACK on every page under PROJECT (the shell does it
// for the path through `project`, UID-851).
//   - Rotary turn moves the focus, push runs the row; a tap focuses and a tap on the focused row runs it (UI-341).
//     Default focus: the last row used in this session.
//   - Name entry is the shell's Dialog (864 × 320 + SoftwareKeyboard over the strip, UID-854) with a live
//     validateProjectName (PRJ-004; primary + DONE disabled while invalid, UID-855): SAVE (first save), SAVE AS
//     (CANCEL · SAVE · SAVE TO USB), CREATE NEW (warns first when dirty, UID-856), SAVE AS PACKAGE, RENAME.
//   - SAVE of a saved project asks OVERWRITE?, then a blocking Progress and a Toast; the list stays (UID-857). SAVE AS
//     onto an existing name asks REPLACE (destructive).
//   - OPEN PROJECT / DELETE PROJECT: a table NAME · MODIFIED · SCENES · LOCATION (USB badge for external projects, the
//     open project in accent with `✓ open`; a tap on NAME or MODIFIED sorts, UID-853); push asks first (OPEN:
//     "Open …?" + unsaved warning; DELETE: destructive, the open project disabled); SHIFT + push opens the project
//     actions list OPEN · RENAME… · DUPLICATE · DELETE… (UID-861; SHOW IN FINDER is host-only, not listed here).
//   - RENDER AUDIO (UID-858…860) is ui/screens/render/ (wired to the offline render service).
// The lifecycle calls go through ProjectSource: StoreProjectSource over services::store::ProjectStore in the product
// (installProjectSession), a fake in the tests.
#include "ui/screens/project/ProjectAccess.h"
#include "ui/screens/project/ProjectSource.h"
#include "ui/screens/sequence/M2Session.h"
#include "ui/shell/Layout.h"
#include "ui/shell/Popup.h"
#include "ui/shell/ShellApi.h"
#include "ui/shell/UiState.h"
#include "ui/theme/Draw.h"
#include "ui/widgets/Button.h"
#include "ui/widgets/ListRow.h"
#include "ui/widgets/Progress.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace ui
{
namespace
{
namespace tk = ::theme::tokens;
namespace L = layout;
using Kind = io::ControlEvent::Kind;
using m2::ProjectEntry;
using m2::ProjectSource;

constexpr RectF kTitle{16, 152, 1248, 48};
constexpr float kRowH = 44;
constexpr std::string_view kRows[] = {"CREATE NEW",      "OPEN",         "SAVE",         "SAVE AS",     "DELETE",
                                      "SAVE AS PACKAGE", "RENDER AUDIO", "EXPORT SCENE", "IMPORT SCENE"};
constexpr int kRowCount = 9;
constexpr const char* kHint = "1\xE2\x80\x93"
                              "64 characters; not / \\ : * ? \" < > |";

struct Memory
{
    int lastRow = 0;
    m2::ProjectSort sort = m2::ProjectSort::Modified;
};
Memory& memory()
{
    static Memory m;
    return m;
}

void toast(ShellApi& shell, std::string_view text)
{
    ToastRequest t;
    t.text.assign(text);
    shell.showToast(std::move(t));
}

void message(ShellApi& shell, glyph::Kind icon, std::string title, std::vector<std::string> lines,
             std::vector<MessageButton> buttons, int defaultButton, std::function<void(int)> onResult)
{
    MessageRequest r;
    r.icon = icon;
    r.title = std::move(title);
    r.lines = std::move(lines);
    r.buttons = std::move(buttons);
    r.defaultButton = defaultButton;
    r.onResult = std::move(onResult);
    shell.showMessage(std::move(r));
}

void error(ShellApi& shell, std::string text)
{
    message(shell, glyph::Kind::Error, "ERROR", {std::move(text)}, {{"OK", ButtonStyle::Primary}}, -1, {});
}

// PanelTitle (project.md "PanelTitle"): 48 tall, surface-2 with radius 8 on the top corners, a border hairline under
// it, the title in `title` at x + 24, an optional summary in body text-2 right-aligned at x + w − 24.
void panelTitle(DrawList& dl, const UiContext& ctx, std::string_view title, std::string_view right)
{
    const auto& p = ctx.palette;
    dl.fill(kTitle, p.surface2, tk::radiusM);
    dl.fill({kTitle.x, kTitle.y + kTitle.h - 8, kTitle.w, 8}, p.surface2);
    draw::hline(dl, kTitle.x, kTitle.x + kTitle.w, kTitle.y + kTitle.h, p.border);
    dl.text(kTitle.x + 24, kTitle.y + kTitle.h / 2, Align::Left, TextStyleId::Title, p.text1, title);
    if (! right.empty())
        dl.text(kTitle.x + kTitle.w - 24, kTitle.y + kTitle.h / 2, Align::Right, TextStyleId::Body, p.text2, right);
}

// The name dialog of project.md "SAVE AS and name dialogs".
void nameDialog(ShellApi& shell, std::string title, std::string prompt, std::string text,
                std::vector<MessageButton> buttons, std::function<void(int, std::string)> onResult)
{
    DialogRequest r;
    r.title = std::move(title);
    r.prompt = std::move(prompt);
    r.text = std::move(text);
    r.hint = kHint;
    r.buttons = std::move(buttons);
    r.primary = 1;
    r.cancel = 0;
    r.validate = [](std::string_view t) -> std::string
    {
        const auto why = m2::validateProjectName(t);
        return why ? std::string(*why) : std::string();
    };
    r.onResult = [onResult = std::move(onResult)](int button, std::string_view t) { onResult(button, std::string(t)); };
    shell.showDialog(std::move(r));
}

// Runs a save with the blocking Progress and the result Toast (UID-857).
void runSave(ShellApi& shell, m2::SaveTarget target, const std::string& name)
{
    ProgressRequest pr;
    pr.title = "SAVING";
    pr.detail = name;
    auto* progress = shell.showProgress(std::move(pr));
    auto live = std::make_shared<Progress*>(progress);
    ProjectSource::current().save(
        target, name,
        [live](float f)
        {
            if (*live != nullptr)
                (*live)->set(f);
        },
        [&shell, live](bool ok, std::string why)
        {
            if (*live != nullptr)
                (*live)->done();
            *live = nullptr;
            if (ok)
                toast(shell, "Saved");
            else
                error(shell, std::move(why));
        });
}

void saveAs(ShellApi& shell, std::string proposal)
{
    nameDialog(shell, "SAVE AS", "Save current PROJECT as:", std::move(proposal),
               {{"CANCEL", ButtonStyle::Plain}, {"SAVE", ButtonStyle::Primary}, {"SAVE TO USB", ButtonStyle::Plain}},
               [&shell](int button, std::string name)
               {
                   if (button == 0)
                       return;
                   const auto target = button == 2 ? m2::SaveTarget::SaveTo : m2::SaveTarget::SaveAs;
                   if (ProjectSource::current().exists(name) && name != ProjectSource::current().currentName())
                   {
                       message(shell, glyph::Kind::Caution, "SAVE AS", {"Replace the existing project " + name + "?"},
                               {{"CANCEL", ButtonStyle::Plain}, {"REPLACE", ButtonStyle::Destructive}}, 0,
                               [&shell, target, name](int b)
                               {
                                   if (b == 1)
                                       runSave(shell, target, name);
                               });
                       return;
                   }
                   runSave(shell, target, name);
               });
}

void createNew(ShellApi& shell)
{
    auto ask = [&shell]
    {
        nameDialog(shell, "CREATE NEW", "Name of the new project:", m2::newProjectName(ProjectSource::current()),
                   {{"CANCEL", ButtonStyle::Plain}, {"CREATE", ButtonStyle::Primary}},
                   [&shell](int button, std::string name)
                   {
                       if (button != 1)
                           return;
                       ProjectSource::current().create(name,
                                                       [&shell](bool ok, std::string why)
                                                       {
                                                           if (! ok)
                                                               error(shell, std::move(why));
                                                           else
                                                               shell.home(); // after CREATE NEW: HOME
                                                       });
                   });
    };
    auto& src = ProjectSource::current();
    if (! src.dirty())
    {
        ask();
        return;
    }
    message(shell, glyph::Kind::Caution, "CREATE NEW",
            {"Create a new project?", "Unsaved changes to " + src.currentName() + " will be lost."},
            {{"CANCEL", ButtonStyle::Plain}, {"OK", ButtonStyle::Primary}}, -1,
            [ask](int b)
            {
                if (b == 1)
                    ask();
            });
}

void save(ShellApi& shell)
{
    auto& src = ProjectSource::current();
    if (! src.hasFile())
    {
        nameDialog(shell, "SAVE", "Save current PROJECT as:", src.currentName(),
                   {{"CANCEL", ButtonStyle::Plain}, {"SAVE", ButtonStyle::Primary}},
                   [&shell](int button, std::string name)
                   {
                       if (button == 1)
                           runSave(shell, m2::SaveTarget::Save, name);
                   });
        return;
    }
    const std::string name = src.currentName();
    message(shell, glyph::Kind::Info, "SAVE", {"Overwrite " + name + "?"},
            {{"CANCEL", ButtonStyle::Plain}, {"OVERWRITE", ButtonStyle::Primary}}, -1,
            [&shell, name](int b)
            {
                if (b == 1)
                    runSave(shell, m2::SaveTarget::Save, name);
            });
}

void notYet(ShellApi& shell, std::string_view what)
{
    toast(shell, std::string(what) + " is not available yet");
}

// --- PROJECT ------------------------------------------------------------------------------------------------------
class ProjectList final : public Widget
{
public:
    explicit ProjectList(const WidgetArgs& a) noexcept : shell_(a.shell), state_(a.state) {}
    RectF bounds() const noexcept override { return {16, L::contentTop, 1248, L::contentH}; }
    int focus() const noexcept { return memory().lastRow; }

    void build(DrawList& dl, const UiContext& ctx) const override
    {
        auto& src = ProjectSource::current();
        src.poll(); // a running save's progress → its Progress popup
        const bool dirty = src.dirty();
        if (state_ != nullptr)
            state_->dirty = dirty;
        const auto& name = src.currentName();
        summary_.format("%.*s%s", static_cast<int>(name.size()), name.data(), dirty ? " \xC2\xB7  edited" : "");
        panelTitle(dl, ctx, "PROJECT", summary_.view());
        for (int i = 0; i < kRowCount; ++i)
        {
            ListRowModel m;
            m.label = kRows[i];
            m.zebra = i;
            m.focused = i == memory().lastRow;
            m.opens = true;
            m.labelX = 40;
            ListRow::draw(dl, ctx, {16, 200 + kRowH * static_cast<float>(i), 1248, kRowH}, m);
        }
    }

    bool handle(const io::ControlEvent& e, UiContext&) override
    {
        if (shell_ == nullptr)
            return false;
        auto& row = memory().lastRow;
        if (e.control == io::Control::Selector && e.kind == Kind::Turn)
        {
            row = std::clamp(row + e.value, 0, kRowCount - 1);
            return true;
        }
        if (e.control == io::Control::SelectorPush && e.kind == Kind::Press)
        {
            run(row);
            return true;
        }
        if (touch::isTouch(e) && e.value == touch::kDown)
        {
            const float y = touch::y(e);
            const int i = static_cast<int>(std::floor((y - 200) / kRowH));
            if (y < 200 || i < 0 || i >= kRowCount)
                return false;
            if (i == row)
                run(i); // a tap on the focused row runs it
            row = i;
            return true;
        }
        return false;
    }

    void run(int row)
    {
        auto& shell = *shell_;
        memory().lastRow = row;
        switch (row)
        {
        case 0:
            createNew(shell);
            break;
        case 1:
            shell.open("project.open");
            break;
        case 2:
            save(shell);
            break;
        case 3:
            saveAs(shell, m2::nextName(ProjectSource::current().currentName()));
            break;
        case 4:
            shell.open("project.delete");
            break;
        case 5:
            nameDialog(shell, "SAVE AS PACKAGE",
                       "Save current PROJECT as a package:", ProjectSource::current().currentName(),
                       {{"CANCEL", ButtonStyle::Plain}, {"SAVE PACKAGE", ButtonStyle::Primary}},
                       [&shell](int b, std::string)
                       {
                           if (b == 1)
                               notYet(shell, "SAVE AS PACKAGE"); // PRJ-007 (not implemented yet)
                       });
            break;
        case 6:
            if (! shell.open("project.renderAudio"))
                notYet(shell, "RENDER AUDIO");
            break;
        default:
            notYet(shell, kRows[row]); // PRJ-008 (P2)
            break;
        }
    }

private:
    ShellApi* shell_;
    UiState* state_;
    mutable FixedText<96> summary_;
};

// --- OPEN PROJECT / DELETE PROJECT -----------------------------------------------------------------------------------
class ProjectTable final : public Widget
{
public:
    static constexpr int kVisible = 8;
    ProjectTable(const WidgetArgs& a, bool deleting) : shell_(a.shell), deleting_(deleting) { refresh(); }
    RectF bounds() const noexcept override { return {16, L::contentTop, 1248, L::contentH}; }

    void refresh()
    {
        rows_ = ProjectSource::current().list(memory().sort);
        if (deleting_) // reference projects (INTERNAL / USB .prj) are read-only: never offered for deletion
            rows_.erase(std::remove_if(rows_.begin(), rows_.end(), [](const ProjectEntry& r) { return r.foreign; }),
                        rows_.end());
        current_ = ProjectSource::current().currentName();
        focus_ = std::clamp(focus_, 0, std::max(0, static_cast<int>(rows_.size()) - 1));
        if (deleting_)
            skipDisabled(1);
        seen_ = m2::Session::instance().revision();
    }
    int focus() const noexcept { return focus_; }
    const std::vector<ProjectEntry>& rows() const noexcept { return rows_; }

    void build(DrawList& dl, const UiContext& ctx) const override
    {
        const auto& p = ctx.palette;
        ProjectSource::current().poll(); // a running load's progress → its Progress popup
        const bool byName = memory().sort == m2::ProjectSort::Name;
        summary_.format("%d project%s \xC2\xB7 sorted by %s \xE2\x8C\x84", static_cast<int>(rows_.size()),
                        rows_.size() == 1 ? "" : "s", byName ? "name" : "date");
        panelTitle(dl, ctx, deleting_ ? "DELETE PROJECT" : "OPEN PROJECT", summary_.view());
        dl.fill({16, 200, 1248, 48}, p.surface3);
        dl.text(56, 224, Align::Left, TextStyleId::Caption, byName ? p.text1 : p.text2,
                byName ? "NAME \xE2\x8C\x84" : "NAME");
        dl.text(716, 224, Align::Left, TextStyleId::Caption, byName ? p.text2 : p.text1,
                byName ? "MODIFIED" : "MODIFIED \xE2\x8C\x84");
        dl.text(956, 224, Align::Left, TextStyleId::Caption, p.text2, "SCENES");
        dl.text(1116, 224, Align::Left, TextStyleId::Caption, p.text2, "LOCATION");
        if (rows_.empty())
        {
            dl.text(640, 400, Align::Centre, TextStyleId::Body, p.text3,
                    deleting_ ? "No projects yet. CREATE NEW makes one."
                              : "No projects here, on the internal disk or a USB stick. CREATE NEW makes one.");
            return;
        }
        for (int i = first_; i < static_cast<int>(rows_.size()) && i < first_ + kVisible; ++i)
        {
            const auto& r = rows_[static_cast<std::size_t>(i)];
            const float y = 248 + kRowH * static_cast<float>(i - first_);
            const bool focused = i == focus_;
            const bool open = r.name == current_ && ! r.foreign;
            const bool disabled = deleting_ && open;
            dl.fill({16, y, 1248, kRowH}, (i - first_) % 2 == 0 ? p.surface2 : p.surface1);
            if (focused)
            {
                dl.fill({16, y, 1248, kRowH}, p.accentWeak);
                dl.fill({16, y, 3, kRowH}, p.accent);
            }
            const float cy = y + kRowH / 2;
            const auto style = focused ? TextStyleId::BodyStrong : TextStyleId::Body;
            dl.text(56, cy, Align::Left, style, disabled ? p.text3 : (open ? p.accent : p.text1), r.name);
            if (open)
            {
                const float w = ctx.measure ? ctx.measure(style, r.name) : 9.0f * static_cast<float>(r.name.size());
                dl.text(56 + w + 16, cy, Align::Left, TextStyleId::Body, disabled ? p.text3 : p.accent,
                        "\xE2\x9C\x93 open");
            }
            dl.text(716, cy, Align::Left, TextStyleId::Body, disabled ? p.text3 : p.text2, r.modified);
            scenes_.format("%d", r.scenes);
            dl.text(956, cy, Align::Left, TextStyleId::Body, disabled ? p.text3 : p.text2, scenes_.view());
            if (r.external)
            {
                const std::string_view badge =
                    r.location.empty() ? std::string_view("USB") : std::string_view(r.location);
                const float w = std::max(48.0f, (ctx.measure ? ctx.measure(TextStyleId::Caption, badge) : 24.0f) + 16);
                dl.fill({1116, y + 11, w, 22}, p.surface3, tk::radiusXs);
                dl.text(1116 + w / 2, y + 23, Align::Centre, TextStyleId::Caption, p.text1, badge);
            }
            else
                dl.text(1116, cy, Align::Left, TextStyleId::Body, p.text3, "Internal");
        }
        // Scrollbar (UID-904): thumb over a 300 lpx track at x 1256.
        const float frac = std::min(1.0f, static_cast<float>(kVisible) / static_cast<float>(rows_.size()));
        const float start = rows_.size() > static_cast<std::size_t>(kVisible)
                                ? static_cast<float>(first_) / static_cast<float>(rows_.size())
                                : 0.0f;
        dl.fill({1256, 252 + 300 * start, 4, std::max(16.0f, 300 * frac)}, p.borderStrong, 2);
    }

    bool handle(const io::ControlEvent& e, UiContext&) override
    {
        if (shell_ == nullptr)
            return false;
        if (m2::Session::instance().revision() != seen_)
            refresh();
        if (e.control == io::Control::Selector && e.kind == Kind::Turn)
        {
            move(e.value);
            return true;
        }
        if (e.control == io::Control::SelectorPush && e.kind == Kind::Press)
        {
            if (shell_->state().shift)
                actions(focus_);
            else
                run(focus_);
            return true;
        }
        if (touch::isTouch(e) && e.value == touch::kDown)
        {
            const float x = touch::x(e), y = touch::y(e);
            if (y >= 200 && y < 248) // header: NAME / MODIFIED sort (PRJ-005)
            {
                if (x < 700)
                    memory().sort = m2::ProjectSort::Name;
                else if (x < 940)
                    memory().sort = m2::ProjectSort::Modified;
                refresh();
                return true;
            }
            const int i = first_ + static_cast<int>(std::floor((y - 248) / kRowH));
            if (y < 248 || i < first_ || i >= static_cast<int>(rows_.size()) || i >= first_ + kVisible)
                return false;
            if (i == focus_)
                run(i);
            else
                focus_ = i;
            return true;
        }
        return false;
    }

private:
    bool disabled(int i) const
    {
        return deleting_ && i >= 0 && i < static_cast<int>(rows_.size()) &&
               rows_[static_cast<std::size_t>(i)].name == current_ && ! rows_[static_cast<std::size_t>(i)].foreign;
    }
    void skipDisabled(int dir)
    {
        const int n = static_cast<int>(rows_.size());
        for (int k = 0; k < n && disabled(focus_); ++k)
            focus_ = std::clamp(focus_ + dir, 0, n - 1);
    }
    void move(int d)
    {
        const int n = static_cast<int>(rows_.size());
        if (n == 0)
            return;
        focus_ = std::clamp(focus_ + d, 0, n - 1);
        if (disabled(focus_))
        {
            skipDisabled(d > 0 ? 1 : -1);
            if (disabled(focus_))
                skipDisabled(d > 0 ? -1 : 1);
        }
        // Keep one row of context around the focus (UID-170).
        if (focus_ < first_ + 1)
            first_ = std::max(0, focus_ - 1);
        if (focus_ > first_ + kVisible - 2)
            first_ = std::min(std::max(0, n - kVisible), focus_ - kVisible + 2);
    }

    void run(int i)
    {
        if (rows_.empty()) // a push on an empty list says so (it did nothing before)
        {
            toast(*shell_, deleting_ ? "No project to delete" : "No project to open");
            return;
        }
        if (i < 0 || i >= static_cast<int>(rows_.size()) || disabled(i))
            return;
        const auto entry = rows_[static_cast<std::size_t>(i)];
        if (deleting_)
            askDelete(entry);
        else
            askOpen(entry);
    }

    void askOpen(const ProjectEntry& entry)
    {
        auto& shell = *shell_;
        auto& src = ProjectSource::current();
        std::vector<std::string> lines{entry.foreign ? "Import " + entry.name + " from " + entry.location
                                                           + " as a new project?"
                                                     : "Open " + entry.name + "?"};
        if (src.dirty())
            lines.push_back("Unsaved changes to " + src.currentName() + " will be lost.");
        message(shell, src.dirty() ? glyph::Kind::Caution : glyph::Kind::Info, "OPEN", std::move(lines),
                {{"CANCEL", ButtonStyle::Plain}, {"OPEN", ButtonStyle::Primary}}, -1,
                [&shell, entry](int b)
                {
                    if (b != 1)
                        return;
                    ProgressRequest pr;
                    pr.title = "LOADING";
                    pr.detail = entry.name;
                    pr.cancellable = true;
                    pr.onCancel = [] { ProjectSource::current().cancel(); };
                    auto live = std::make_shared<Progress*>(shell.showProgress(std::move(pr)));
                    ProjectSource::current().open(
                        entry,
                        [live](float f)
                        {
                            if (*live != nullptr)
                                (*live)->set(f);
                        },
                        [&shell, live](bool ok, std::string why, std::vector<std::string> missing)
                        {
                            if (*live != nullptr)
                                (*live)->done();
                            *live = nullptr;
                            if (! ok)
                            {
                                error(shell, std::move(why));
                                return;
                            }
                            shell.home(); // the saved screen is restored by the project session's Resume (PRJ-023)
                            if (! missing.empty())
                            {
                                std::vector<std::string> names;
                                for (std::size_t k = 0; k < missing.size() && k < 6; ++k)
                                    names.push_back(missing[k]);
                                if (missing.size() > 6)
                                    names.push_back("and " + std::to_string(missing.size() - 6) + " more");
                                message(shell, glyph::Kind::Caution, "MISSING SAMPLES", std::move(names),
                                        {{"OK", ButtonStyle::Primary}}, -1, {});
                            }
                        });
                });
    }

    void askDelete(const ProjectEntry& entry)
    {
        auto& shell = *shell_;
        message(shell, glyph::Kind::Caution, "DELETE", {"Move " + entry.name + " to the Trash?"},
                {{"CANCEL", ButtonStyle::Plain}, {"DELETE", ButtonStyle::Destructive}}, 0,
                [this, entry](int b)
                {
                    if (b != 1)
                        return;
                    std::string why;
                    if (! ProjectSource::current().remove(entry, why))
                        error(*shell_, why);
                    else
                        toast(*shell_, entry.name + " moved to the Trash");
                    refresh();
                });
    }

    void actions(int i)
    {
        if (i < 0 || i >= static_cast<int>(rows_.size()))
            return;
        const auto entry = rows_[static_cast<std::size_t>(i)];
        ListRequest r;
        r.title = "PROJECT";
        const bool writable = ! entry.foreign; // a reference project (INTERNAL / USB .prj) is read-only
        r.items = {{"OPEN", {}, {}, 0},
                   {"RENAME\xE2\x80\xA6", {}, {}, 1, writable},
                   {"DUPLICATE", {}, {}, 2, writable},
                   {"DELETE\xE2\x80\xA6", {}, {}, 3, writable && entry.name != current_}};
        r.onChoose = [this, entry](int, const ListItem& item)
        {
            auto& shell = *shell_;
            std::string why;
            switch (item.value)
            {
            case 0:
                askOpen(entry);
                break;
            case 1:
                nameDialog(shell, "RENAME", "New name of " + entry.name + ":", entry.name,
                           {{"CANCEL", ButtonStyle::Plain}, {"RENAME", ButtonStyle::Primary}},
                           [this, entry](int b, std::string name)
                           {
                               std::string e;
                               if (b == 1 && ! ProjectSource::current().rename(entry, name, e))
                                   error(*shell_, e);
                               refresh();
                           });
                break;
            case 2:
                if (! ProjectSource::current().duplicate(entry, why))
                    error(shell, why);
                refresh();
                break;
            default:
                askDelete(entry);
                break;
            }
        };
        shell_->showList(std::move(r));
    }

    ShellApi* shell_;
    bool deleting_;
    std::vector<ProjectEntry> rows_;
    std::string current_;
    int focus_ = 0, first_ = 0;
    std::uint32_t seen_ = 0;
    mutable FixedText<64> summary_;
    mutable FixedText<8> scenes_;
};

std::unique_ptr<Widget> makeProjectList(const ScreenSpec&, const WidgetArgs& a)
{
    return std::make_unique<ProjectList>(a);
}
std::unique_ptr<Widget> makeProjectTable(const ScreenSpec& s, const WidgetArgs& a)
{
    return std::make_unique<ProjectTable>(a, s.id == "project.delete");
}
} // namespace

int projectFocus(Widget* body)
{
    if (auto* l = dynamic_cast<ProjectList*>(body))
        return l->focus();
    if (auto* t = dynamic_cast<ProjectTable*>(body))
        return t->focus();
    return -1;
}
void resetProjectScreens()
{
    memory() = Memory{};
}
} // namespace ui

APP_REGISTER_WIDGET("ProjectList", &ui::makeProjectList);
APP_REGISTER_WIDGET("ProjectTable", &ui::makeProjectTable);
APP_REGISTER_SCREEN((ui::ScreenSpec{.id = "project",
                                    .title = "PROJECT",
                                    .parent = "home",
                                    .body = ui::BodyTemplate::Full,
                                    .contextBar = ui::ContextBarKind::Menu,
                                    .rail = {},
                                    .bodyComponent = "ProjectList",
                                    .strip = {"PROJECT", {}, 0}}));
APP_REGISTER_SCREEN((ui::ScreenSpec{.id = "project.open",
                                    .title = "OPEN PROJECT",
                                    .parent = "project",
                                    .body = ui::BodyTemplate::Full,
                                    .contextBar = ui::ContextBarKind::Menu,
                                    .rail = {},
                                    .bodyComponent = "ProjectTable",
                                    .strip = {"OPEN PROJECT", {}, 0}}));
APP_REGISTER_SCREEN((ui::ScreenSpec{.id = "project.delete",
                                    .title = "DELETE PROJECT",
                                    .parent = "project",
                                    .body = ui::BodyTemplate::Full,
                                    .contextBar = ui::ContextBarKind::Menu,
                                    .rail = {},
                                    .bodyComponent = "ProjectTable",
                                    .strip = {"DELETE PROJECT", {}, 0}}));
// RENDER AUDIO (project.renderAudio): ui/screens/render/RenderScreen.cpp.
