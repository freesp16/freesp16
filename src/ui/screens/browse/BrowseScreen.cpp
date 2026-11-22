// BROWSE (07-ui-design/screens/browse.md; UI-310…315, UI-346, UID-675…686). Template rail+content with the compact
// five-item rail, context bar trackContext. ListTable tree of the library's locations (virtualised, 9 rows); push on
// a folder expands / collapses, push on a file loads it onto the selected track (inline progress, never blocking,
// UID-686); SHIFT + turn jumps folder to folder (LIB-017); PREVIEW on plays the focused file (UI-313); the assigned
// file is accent with ✓; USB marks removable files; UNLOAD acts at once with a toast, undoable (UID-683). FILTER TYPE
// = TEXT opens the shell's text dialog (typed with a hardware / host keyboard until the on-screen keyboard).
// The compact rail is the shell's; every item runs this screen's action (RailOwner: FILTER TYPE list, PREVIEW,
// SETTING popup, ASSIGNED SAMPLE, UNLOAD) and its state (value, on, disabled). Library access goes through
// m1::BrowseSource (adapter over services::library).
#include "ui/screens/browse/BrowseSource.h"
#include "ui/screens/home/TrackInfo.h"
#include "ui/shell/Layout.h"
#include "ui/shell/Popup.h"
#include "ui/shell/RailOwner.h"
#include "ui/widgets/ListTable.h"
#include "ui/widgets/Toast.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace ui
{
namespace
{
namespace L = layout;
using Kind = io::ControlEvent::Kind;
using m1::BrowseEntry;
using m1::BrowseSource;

class BrowseBody final : public Widget, public RailOwner
{
public:
    explicit BrowseBody(const WidgetArgs& a) : shell_(a.shell), state_(a.state), alive_(std::make_shared<bool>(true))
    {
        rows_.reserve(256);
        models_.reserve(256);
        if (shell_ != nullptr && state_ != nullptr)
            reload();
    }
    ~BrowseBody() override
    {
        *alive_ = false;
        if (auto* src = BrowseSource::installed(); src != nullptr && previewPath_.size())
            src->preview({});
    }
    RectF bounds() const noexcept override { return {16, L::contentTop, 1248, L::contentH}; }
    int focusedRow() const noexcept { return focus_; }
    const std::vector<TreeRowModel>& rows() const noexcept { return models_; }

    void build(DrawList& dl, const UiContext& ctx) const override
    {
        if (shell_ == nullptr || state_ == nullptr)
            return;
        auto* src = BrowseSource::installed();
        const int t = state_->selectedTrack;
        if (t != track_) // selecting another track retargets the assigned mark (browse.md "Interactions")
        {
            track_ = t;
            markAssigned();
        }
        const bool text = filterText();
        auto& sh = *shell_;
        ListTableModel m;
        m.title = text ? std::string_view(textTitle_) : std::string_view("FOLDER");
        m.sort = m1::trackParam(sh, "browse.sort", t) >= 0.5f ? "Z\xE2\x86\x92"
                                                                "A"
                                                              : "A\xE2\x86\x92Z";
        m.freeSpace = freeSpace_;
        m.rows = models_;
        m.first = first_;
        m.focused = focus_;
        m.previewing = src != nullptr && src->previewing() && ! previewPath_.empty();
        m.loading = loadingRow_ >= 0 ? m1::loadProgress()[t - 1] : -1.0f;
        m.loadingRow = loadingRow_;
        ListTable::draw(dl, ctx, m);
    }

    bool dirty(double) const override { return loadingRow_ >= 0; }

    bool handle(const io::ControlEvent& e, UiContext&) override
    {
        if (shell_ == nullptr || state_ == nullptr)
            return false;
        if (e.control == io::Control::Selector && e.kind == Kind::Turn)
        {
            if (state_->shift) // SHIFT + turn: folder to folder (LIB-017)
            {
                int i = focus_;
                for (int n = 0; n < std::abs(e.value); ++n)
                {
                    int j = i + (e.value > 0 ? 1 : -1);
                    while (j >= 0 && j < static_cast<int>(rows_.size()) &&
                           ! rows_[static_cast<std::size_t>(j)].e.folder)
                        j += e.value > 0 ? 1 : -1;
                    if (j < 0 || j >= static_cast<int>(rows_.size()))
                        break;
                    i = j;
                }
                setFocus(i);
            }
            else
                setFocus(focus_ + e.value);
            return true;
        }
        if (e.control == io::Control::SelectorPush && e.kind == Kind::Press)
        {
            activate(focus_);
            return true;
        }
        if (! touch::isTouch(e) || e.value != touch::kDown)
            return false;
        const float x = touch::x(e), y = touch::y(e);
        if (contains({ListTable::kPanel.x, ListTable::kRowY0, ListTable::kPanel.w, ListTable::kRowH * 9}, x, y))
        {
            const int row = first_ + static_cast<int>((y - ListTable::kRowY0) / ListTable::kRowH);
            if (row >= static_cast<int>(rows_.size()))
                return true;
            if (row == focus_)
                activate(row); // click the focused row = load / expand (UI-341)
            else
                setFocus(row);
            return true;
        }
        return contains(bounds(), x, y);
    }

private:
    struct Node
    {
        BrowseEntry e;
        int level = 0;
        bool open = false;
    };

    bool filterText() const { return m1::trackParam(*shell_, "browse.filterType", state_->selectedTrack) >= 0.5f; }

    void reload()
    {
        rows_.clear();
        if (auto* src = BrowseSource::installed())
        {
            for (auto& r : src->roots())
                rows_.push_back({std::move(r), 0, false});
            track_ = state_->selectedTrack;
            const auto assigned = src->assigned(track_);
            rebuild();
            setFocus(0);
            if (! assigned.empty())
                expandTo(assigned); // LIB-018 applied on entry
        }
        else
            rebuild();
    }

    void rebuild() const
    {
        models_.clear();
        auto* src = BrowseSource::installed();
        if (src == nullptr)
        {
            models_.push_back({0, TreeRowModel::Kind::Message, "Library not available", {}, {}, false, false, true});
            assignedEmpty_ = true;
            return;
        }
        const auto& assigned = src->assigned(state_->selectedTrack);
        assignedEmpty_ = assigned.empty();
        for (const auto& n : rows_)
        {
            TreeRowModel m;
            m.level = n.level;
            m.kind = n.e.folder ? (n.open ? TreeRowModel::Kind::OpenFolder : TreeRowModel::Kind::ClosedFolder)
                                : TreeRowModel::Kind::File;
            m.name = n.e.name;
            m.badge = n.level == 0 ? std::string_view(n.e.badge) : std::string_view{};
            m.usb = ! n.e.folder && n.e.removable;
            m.assigned = ! n.e.folder && n.e.path == assigned;
            models_.push_back(m);
        }
    }
    void markAssigned() const { rebuild(); }

    void setFocus(int i)
    {
        if (rows_.empty())
            return;
        focus_ = std::clamp(i, 0, static_cast<int>(rows_.size()) - 1);
        first_ = ListTable::scrollFor(focus_, first_, static_cast<int>(rows_.size()));
        const auto& n = rows_[static_cast<std::size_t>(focus_)];
        if (auto* src = BrowseSource::installed())
        {
            freeSpace_ = src->freeSpace(n.e.path);
            const bool preview = m1::trackFlag(*shell_, "browse.preview", state_->selectedTrack, true);
            const std::string next = ! n.e.folder && preview ? n.e.path : std::string{};
            if (next != previewPath_) // moving away stops the preview (UI-313)
            {
                previewPath_ = next;
                src->preview(previewPath_);
            }
        }
    }

    void expand(int i, std::function<void()> then = {})
    {
        auto* src = BrowseSource::installed();
        if (src == nullptr || i < 0 || i >= static_cast<int>(rows_.size()) ||
            ! rows_[static_cast<std::size_t>(i)].e.folder)
            return;
        auto& n = rows_[static_cast<std::size_t>(i)];
        if (n.open)
        {
            // collapse: drop every deeper row below it
            std::size_t j = static_cast<std::size_t>(i) + 1;
            while (j < rows_.size() && rows_[j].level > n.level)
                ++j;
            rows_.erase(rows_.begin() + i + 1, rows_.begin() + static_cast<std::ptrdiff_t>(j));
            n.open = false;
            rebuild();
            setFocus(i);
            return;
        }
        const auto path = n.e.path;
        const auto sort = m1::trackParam(*shell_, "browse.sort", state_->selectedTrack) >= 0.5f
                              ? m1::BrowseSort::NameDown
                              : m1::BrowseSort::NameUp;
        std::weak_ptr<bool> alive = alive_;
        src->list(path, sort,
                  [this, alive, path, then](std::vector<BrowseEntry> entries)
                  {
                      if (alive.expired() || ! *alive.lock())
                          return;
                      const auto it =
                          std::find_if(rows_.begin(), rows_.end(), [&](const Node& x) { return x.e.path == path; });
                      if (it == rows_.end() || it->open)
                          return;
                      it->open = true;
                      const int level = it->level + 1;
                      const auto at = it - rows_.begin() + 1;
                      std::vector<Node> add;
                      for (auto& e : entries)
                          add.push_back({std::move(e), level, false});
                      if (add.empty())
                      {
                          BrowseEntry e;
                          e.name = "Empty";
                          e.path = path + "/";
                          add.push_back({std::move(e), level, false});
                      }
                      rows_.insert(rows_.begin() + at, std::make_move_iterator(add.begin()),
                                   std::make_move_iterator(add.end()));
                      rebuild();
                      if (then)
                          then();
                  });
    }

    // Opens the tree down to `path` and focuses it.
    void expandTo(const std::string& path)
    {
        for (int i = 0; i < static_cast<int>(rows_.size()); ++i)
        {
            const auto& n = rows_[static_cast<std::size_t>(i)];
            if (n.e.path == path)
            {
                setFocus(i);
                return;
            }
            if (n.e.folder && ! n.open && path.rfind(n.e.path + "/", 0) == 0)
            {
                expand(i, [this, path] { expandTo(path); });
                return;
            }
        }
    }

    void activate(int i)
    {
        if (i < 0 || i >= static_cast<int>(rows_.size()))
            return;
        const auto& n = rows_[static_cast<std::size_t>(i)];
        if (n.e.folder)
        {
            expand(i);
            return;
        }
        if (n.e.path.empty() || n.e.name == "Empty")
            return;
        load(i);
    }

    void load(int i)
    {
        auto* src = BrowseSource::installed();
        if (src == nullptr)
            return;
        const int t = state_->selectedTrack;
        loadingRow_ = i;
        m1::loadProgress()[t - 1] = 0;
        std::weak_ptr<bool> alive = alive_;
        src->load(
            t, rows_[static_cast<std::size_t>(i)].e.path,
            [t](float p) { m1::loadProgress()[t - 1] = std::clamp(p, 0.0f, 1.0f); },
            [this, alive, t](bool ok, std::string message)
            {
                m1::loadProgress()[t - 1] = -1;
                if (alive.expired() || ! *alive.lock())
                    return;
                loadingRow_ = -1;
                if (! ok)
                {
                    MessageRequest req;
                    req.icon = glyph::Kind::Error;
                    req.title = "Can't load";
                    req.lines = {std::move(message)};
                    req.buttons = {{"OK", ButtonStyle::Primary}};
                    shell_->showMessage(std::move(req));
                }
                rebuild();
            });
    }

    // --- RailOwner: every item is this screen's action ---------------------------------------------------------------
    bool railActivate(int index, const RailItemSpec&, float, float) override
    {
        if (shell_ == nullptr || state_ == nullptr)
            return false;
        railAction(index);
        return true;
    }
    void railState(int index, const RailItemSpec&, RailItemState& st) const override
    {
        if (shell_ == nullptr || state_ == nullptr)
            return;
        const int t = state_->selectedTrack;
        const bool empty = assignedEmpty_; // cached by rebuild(): no string per frame
        switch (index)
        {
        case 0:
            st.value = m1::trackLabel(*shell_, "browse.filterType", t);
            break;
        case 1:
            st.on = m1::trackFlag(*shell_, "browse.preview", t, true);
            break;
        case 3:
            st.disabled = empty || filterText();
            break;
        case 4:
            st.disabled = empty;
            break;
        default:
            break;
        }
    }

    void railAction(int i)
    {
        auto* src = BrowseSource::installed();
        auto& sh = *shell_;
        const int t = state_->selectedTrack;
        switch (i)
        {
        case 0: // FILTER TYPE: FOLDER / TEXT (TEXT opens the search dialog)
        {
            ListRequest req;
            req.title = "FILTER TYPE";
            req.items = {{"FOLDER", {}, {}, 0}, {"TEXT", {}, {}, 1}};
            req.current = filterText() ? 1 : 0;
            req.onChoose = [this](int, const ListItem& it)
            {
                m1::setTrackParam(*shell_, "browse.filterType", state_->selectedTrack, static_cast<float>(it.value));
                if (it.value == 1)
                    openSearch();
                else
                    reload();
            };
            sh.showList(std::move(req));
            break;
        }
        case 1:
            m1::setTrackParam(sh, "browse.preview", t, m1::trackFlag(sh, "browse.preview", t, true) ? 0.0f : 1.0f);
            previewPath_ = "?"; // re-evaluate
            setFocus(focus_);
            break;
        case 2:
        {
            SettingRequest req;
            req.title = "SETTING";
            if (const auto* mon = sh.params().find("browse.previewMonitor"))
            {
                SettingRow row;
                row.label = "PREVIEW MONITOR";
                row.options.assign(mon->enumLabels.begin(), mon->enumLabels.end());
                row.current = static_cast<int>(std::lround(m1::trackParam(sh, "browse.previewMonitor", t)));
                req.rows.push_back(std::move(row));
            }
            if (const auto* vol = sh.params().find("browse.previewVolume"))
            {
                SettingRow row;
                row.label = "PREVIEW VOLUME";
                row.spec = vol;
                row.value = m1::trackParam(sh, "browse.previewVolume", t, 50);
                req.rows.push_back(std::move(row));
            }
            req.onCommit = [this](int row, int option, float value)
            {
                const int tr = state_->selectedTrack;
                if (row == 0)
                    m1::setTrackParam(*shell_, "browse.previewMonitor", tr, static_cast<float>(option));
                else
                    m1::setTrackParam(*shell_, "browse.previewVolume", tr, value);
            };
            sh.showSetting(std::move(req));
            break;
        }
        case 3: // ASSIGNED SAMPLE: jump to the track's file (LIB-018)
            if (src != nullptr && ! filterText() && ! src->assigned(t).empty())
            {
                reload(); // collapse to the roots, then open down to it
            }
            break;
        case 4: // UNLOAD: at once, undoable, with a toast (UID-683)
            if (src != nullptr && ! src->assigned(t).empty())
            {
                src->unload(t);
                ToastRequest req;
                req.text = "Unloaded";
                sh.showToast(std::move(req));
                rebuild();
            }
            break;
        default:
            break;
        }
    }

    void openSearch()
    {
        DialogRequest req;
        req.title = "Search samples";
        req.text = query_;
        req.buttons = {{"CANCEL"}, {"GO", ButtonStyle::Primary}};
        std::weak_ptr<bool> alive = alive_;
        req.onResult = [this, alive](int button, std::string_view text)
        {
            if (alive.expired() || button != 1)
                return;
            query_.assign(text);
            textTitle_ = "TEXT: \"" + query_ + "\"";
            if (auto* src = BrowseSource::installed())
                src->search(query_,
                            [this, alive](std::vector<BrowseEntry> hits)
                            {
                                if (alive.expired())
                                    return;
                                rows_.clear();
                                for (auto& h : hits)
                                    rows_.push_back({std::move(h), 0, false});
                                if (rows_.empty())
                                {
                                    BrowseEntry none;
                                    none.name = "No samples match \"" + query_ + "\"";
                                    rows_.push_back({std::move(none), 0, false});
                                }
                                first_ = focus_ = 0;
                                rebuild();
                            });
        };
        shell_->showDialog(std::move(req));
    }

    ShellApi* shell_;
    UiState* state_;
    std::shared_ptr<bool> alive_;
    std::vector<Node> rows_;
    mutable std::vector<TreeRowModel> models_;
    int focus_ = 0, first_ = 0, loadingRow_ = -1;
    mutable int track_ = 0;
    mutable bool assignedEmpty_ = true;
    std::string freeSpace_, previewPath_, query_, textTitle_;
};

std::unique_ptr<Widget> makeListTable(const ScreenSpec&, const WidgetArgs& a)
{
    return std::make_unique<BrowseBody>(a);
}
} // namespace
} // namespace ui

namespace ui
{
namespace
{
constexpr RailItemSpec kBrowseRail[] = {{RailItemSpec::Kind::Option, "FILTER TYPE"},
                                        {RailItemSpec::Kind::Toggle, "PREVIEW"},
                                        {RailItemSpec::Kind::Button, "SETTING"},
                                        {RailItemSpec::Kind::Button, "ASSIGNED SAMPLE"},
                                        {RailItemSpec::Kind::Button, "UNLOAD"}};
} // namespace
} // namespace ui

APP_REGISTER_WIDGET("ListTable", &ui::makeListTable)
APP_REGISTER_SCREEN((ui::ScreenSpec{.id = "track.browse",
                                    .title = "BROWSE",
                                    .parent = "track.menu",
                                    .body = ui::BodyTemplate::RailContent,
                                    .contextBar = ui::ContextBarKind::TrackContext,
                                    .rail = ui::kBrowseRail,
                                    .bodyComponent = "ListTable",
                                    .strip = {"BROWSE",
                                              {"browse.filterType", "browse.sort", "browse.preview",
                                               "browse.previewVolume", "browse.loadTo", "browse.info"}}}))
