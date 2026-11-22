// Model paths ↔ ParamPath (data-model.md "Identity and paths"). Our own grammar, indices 0-based,
// written without leading zeros:
//
//   Project   <field>                                   "settings.tempo" (id "project.tempo"), "arrangement.play"
//   Global    preferences.<id>                           a preference (ParamScope::Global), held in Scope::Project
//   Scene     scene[s].<field>                           id "scene.<field>", or any other Scene-scope id verbatim
//   Pattern   scene[s].pattern[p].<field>                id "pattern.<field>" (ParamScope::Scene)
//   SendFx    scene[s].sendFx.<effect>.<key>             id "send.<effect>.<key>"
//   MasterFx  scene[s].masterFx.<effect>.<key>           id "master.<effect>.<key>"
//   Track     scene[s].track[t].<field>                  id "track.<field>", or "playback.pitch" … verbatim
//   InsertFx  scene[s].track[t].insertFx[k].<effect>.<key>   id "fx1.…" (k = 0) / "fx2.…" (k = 1)
//   Step      scene[s].pattern[p].step[i].track[t].<track or insertFx form>   lockable Track-scope ids only (p-locks)
//
// SceneOverrideElseProject ids (AFS-007 scene BPM) are Scene paths. The raw storage form
// "…insertFx[k].params[n]" names an EffectSlot field, not a ParamSpec: its meaning depends on the slot's current
// effect (EffectDescriptor page order), so parsePath does not accept it.
#include "core/model/paths/ParamPath.h"
#include "core/model/Pattern.h"
#include "core/params/ParamRegistry.h"

#include <cstdio>

namespace core::model
{
namespace
{
// Where a ParamSpec lives in the path tree: the scope its paths have, the insert slot an fx1/fx2 id names, and the
// text after the indexed prefix (lead + tail).
struct Form
{
    Scope scope = Scope::Project;
    std::uint8_t slot = 0;
    std::string_view lead {};
    std::string_view tail {};
};

bool startsWith(std::string_view s, std::string_view p) noexcept { return s.substr(0, p.size()) == p; }

bool consume(std::string_view& s, std::string_view p) noexcept
{
    if (! startsWith(s, p))
        return false;
    s.remove_prefix(p.size());
    return true;
}

constexpr std::string_view kPreferences = "preferences.";

std::optional<Form> formOf(const ParamSpec& spec) noexcept
{
    const std::string_view id = spec.id;
    std::string_view rest = id;
    switch (spec.scope)
    {
        case ParamScope::Global: return Form {Scope::Project, 0, kPreferences, id};
        case ParamScope::Project:
            if (consume(rest, "project."))
                return Form {Scope::Project, 0, "settings.", rest};
            return Form {Scope::Project, 0, {}, id};
        case ParamScope::Track:
            if (consume(rest, "fx1."))
                return Form {Scope::InsertFx, 0, {}, rest};
            if (consume(rest, "fx2."))
                return Form {Scope::InsertFx, 1, {}, rest};
            if (consume(rest, "track."))
                return Form {Scope::Track, 0, {}, rest};
            return Form {Scope::Track, 0, {}, id};
        case ParamScope::Scene:
        case ParamScope::SceneOverrideElseProject:
            if (consume(rest, "send."))
                return Form {Scope::SendFx, 0, {}, rest};
            if (consume(rest, "master."))
                return Form {Scope::MasterFx, 0, {}, rest};
            if (consume(rest, "pattern."))
                return Form {Scope::Pattern, 0, {}, rest};
            if (consume(rest, "scene."))
                return Form {Scope::Scene, 0, {}, rest};
            return Form {Scope::Scene, 0, {}, id};
    }
    return std::nullopt;
}

// "[n]" with n in 0…limit−1, canonical decimal (no sign, no leading zeros).
bool consumeIndex(std::string_view& s, int limit, std::uint8_t& out) noexcept
{
    if (s.size() < 3 || s[0] != '[')
        return false;
    std::size_t i = 1;
    int v = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9' && i <= 3)
    {
        v = v * 10 + (s[i] - '0');
        ++i;
    }
    const std::size_t digits = i - 1;
    if (digits == 0 || i >= s.size() || s[i] != ']' || (digits > 1 && s[1] == '0') || v >= limit)
        return false;
    out = static_cast<std::uint8_t>(v);
    s.remove_prefix(i + 1);
    return true;
}

bool consumeNode(std::string_view& s, std::string_view name, int limit, std::uint8_t& out) noexcept
{
    std::string_view t = s;
    if (! consume(t, name) || ! consumeIndex(t, limit, out) || ! consume(t, "."))
        return false;
    s = t;
    return true;
}

bool matches(const Form& f, std::string_view rest) noexcept
{
    return rest.size() == f.lead.size() + f.tail.size() && startsWith(rest, f.lead) &&
           rest.substr(f.lead.size()) == f.tail;
}

// Finds the spec whose form has `scope` (and `slot` for insert FX) and whose lead + tail equal `rest`.
std::optional<ParamId> find(const ParamRegistry& registry, Scope scope, std::uint8_t slot, std::string_view rest,
                            bool lockableOnly) noexcept
{
    if (rest.empty())
        return std::nullopt;
    const auto specs = registry.all();
    for (std::size_t i = 0; i < specs.size(); ++i)
    {
        const auto form = formOf(specs[i]);
        if (! form || form->scope != scope || (scope == Scope::InsertFx && form->slot != slot))
            continue;
        if (lockableOnly && ! specs[i].lockable)
            continue;
        if (matches(*form, rest))
            return ParamId {static_cast<std::uint16_t>(i)};
    }
    return std::nullopt;
}
} // namespace

std::optional<ParamPath> parsePath(std::string_view text, const ParamRegistry& registry)
{
    ParamPath p {Scope::Project, 0, 0, 0, 0, 0, {}};
    std::string_view s = text;
    std::optional<ParamId> id;

    if (consumeNode(s, "scene", kScenes, p.scene))
    {
        if (consumeNode(s, "track", kTracks, p.track))
        {
            if (consumeNode(s, "insertFx", 2, p.slot))
            {
                p.scope = Scope::InsertFx;
                id = find(registry, Scope::InsertFx, p.slot, s, false);
            }
            else
            {
                p.scope = Scope::Track;
                id = find(registry, Scope::Track, 0, s, false);
            }
        }
        else if (consumeNode(s, "pattern", kPatterns, p.pattern))
        {
            if (consumeNode(s, "step", kSteps, p.step))
            {
                if (! consumeNode(s, "track", kTracks, p.track))
                    return std::nullopt;
                p.scope = Scope::Step;
                if (consumeNode(s, "insertFx", 2, p.slot))
                    id = find(registry, Scope::InsertFx, p.slot, s, true);
                else
                    id = find(registry, Scope::Track, 0, s, true);
            }
            else
            {
                p.scope = Scope::Pattern;
                id = find(registry, Scope::Pattern, 0, s, false);
            }
        }
        else if (consume(s, "sendFx."))
        {
            p.scope = Scope::SendFx;
            id = find(registry, Scope::SendFx, 0, s, false);
        }
        else if (consume(s, "masterFx."))
        {
            p.scope = Scope::MasterFx;
            id = find(registry, Scope::MasterFx, 0, s, false);
        }
        else
        {
            p.scope = Scope::Scene;
            id = find(registry, Scope::Scene, 0, s, false);
        }
    }
    else if (startsWith(s, "scene["))
        return std::nullopt; // a malformed scene index never falls back to a project field
    else
        id = find(registry, Scope::Project, 0, s, false);

    if (! id)
        return std::nullopt;
    p.param = *id;
    return p;
}

std::string formatPath(const ParamPath& p, const ParamRegistry& registry)
{
    if (! p.param.valid() || p.param.value >= registry.size())
        return {};
    const auto& spec = registry[p.param];
    const auto form = formOf(spec);
    if (! form || p.scene >= kScenes || p.track >= kTracks || p.pattern >= kPatterns || p.step >= kSteps)
        return {};

    char buf[256];
    int n = -1;
    const auto lead = static_cast<int>(form->lead.size());
    const auto tail = static_cast<int>(form->tail.size());
    const char* l = form->lead.data();
    const char* t = form->tail.data();
    switch (p.scope)
    {
        case Scope::Project:
            if (form->scope == Scope::Project)
                n = std::snprintf(buf, sizeof buf, "%.*s%.*s", lead, l, tail, t);
            break;
        case Scope::Scene:
            if (form->scope == Scope::Scene)
                n = std::snprintf(buf, sizeof buf, "scene[%d].%.*s", p.scene, tail, t);
            break;
        case Scope::Pattern:
            if (form->scope == Scope::Pattern)
                n = std::snprintf(buf, sizeof buf, "scene[%d].pattern[%d].%.*s", p.scene, p.pattern, tail, t);
            break;
        case Scope::SendFx:
            if (form->scope == Scope::SendFx)
                n = std::snprintf(buf, sizeof buf, "scene[%d].sendFx.%.*s", p.scene, tail, t);
            break;
        case Scope::MasterFx:
            if (form->scope == Scope::MasterFx)
                n = std::snprintf(buf, sizeof buf, "scene[%d].masterFx.%.*s", p.scene, tail, t);
            break;
        case Scope::Track:
            if (form->scope == Scope::Track)
                n = std::snprintf(buf, sizeof buf, "scene[%d].track[%d].%.*s", p.scene, p.track, tail, t);
            break;
        case Scope::InsertFx:
            if (form->scope == Scope::InsertFx && p.slot == form->slot)
                n = std::snprintf(buf, sizeof buf, "scene[%d].track[%d].insertFx[%d].%.*s", p.scene, p.track, p.slot,
                                  tail, t);
            break;
        case Scope::Step:
            if (! spec.lockable)
                break;
            if (form->scope == Scope::Track)
                n = std::snprintf(buf, sizeof buf, "scene[%d].pattern[%d].step[%d].track[%d].%.*s", p.scene,
                                  p.pattern, p.step, p.track, tail, t);
            else if (form->scope == Scope::InsertFx && p.slot == form->slot)
                n = std::snprintf(buf, sizeof buf, "scene[%d].pattern[%d].step[%d].track[%d].insertFx[%d].%.*s",
                                  p.scene, p.pattern, p.step, p.track, p.slot, tail, t);
            break;
    }
    if (n <= 0 || static_cast<std::size_t>(n) >= sizeof buf)
        return {};
    return std::string(buf, static_cast<std::size_t>(n));
}
} // namespace core::model
