#include "core/commands/arrangement/ArrangementCommands.h"

#include "core/model/enums/ValueLists.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace core::commands::arrangement
{
namespace
{
using model::Arrangement;
using model::Project;
using model::Section;

std::size_t at(int i) noexcept
{
    return static_cast<std::size_t>(i);
}

void syncLength(Arrangement& a) noexcept
{
    a.length = static_cast<int>(a.sections.size()); // ArrangementData.length = the row count (our choice)
}

std::size_t rowBytes(const Section& s) noexcept
{
    std::size_t n = sizeof(Section);
    for (const auto& e : s.extras.entries)
        n += e.key.capacity() + e.value.capacity();
    return n;
}

class InsertRow final : public Command
{
public:
    InsertRow(int row, Section s, std::string_view label) : row_(row), section_(std::move(s)), label_(label) {}
    std::string_view label() const override { return label_; }
    void apply(Project& p) override
    {
        auto& v = p.arrangement.sections;
        v.insert(v.begin() + static_cast<std::ptrdiff_t>(std::min(at(row_), v.size())), section_);
        syncLength(p.arrangement);
    }
    std::unique_ptr<Command> inverse(const Project&) const override;
    void collectChanges(ChangeSet& c) const override { c.add(std::string(kSectionsPath)); }
    std::size_t sizeBytes() const override { return sizeof(*this) + rowBytes(section_); }

private:
    int row_;
    Section section_;
    std::string_view label_;
};

class DeleteRow final : public Command
{
public:
    DeleteRow(int row, std::string_view label) : row_(row), label_(label) {}
    std::string_view label() const override { return label_; }
    void apply(Project& p) override
    {
        auto& v = p.arrangement.sections;
        if (at(row_) < v.size())
            v.erase(v.begin() + static_cast<std::ptrdiff_t>(row_));
        syncLength(p.arrangement);
    }
    std::unique_ptr<Command> inverse(const Project& before) const override
    {
        return std::make_unique<InsertRow>(row_, before.arrangement.sections[at(row_)], label_);
    }
    void collectChanges(ChangeSet& c) const override { c.add(std::string(kSectionsPath)); }

private:
    int row_;
    std::string_view label_;
};

std::unique_ptr<Command> InsertRow::inverse(const Project&) const
{
    return std::make_unique<DeleteRow>(row_, label_);
}

// Replaces one row; `key` names a knob edit (row + field) that coalesces with the next one ("" = never).
class SetRow final : public Command
{
public:
    SetRow(int row, Section s, std::string_view label, std::string key)
        : row_(row), section_(std::move(s)), label_(label), key_(std::move(key))
    {
    }
    std::string_view label() const override { return label_; }
    void apply(Project& p) override
    {
        if (at(row_) < p.arrangement.sections.size())
            p.arrangement.sections[at(row_)] = section_;
    }
    std::unique_ptr<Command> inverse(const Project& before) const override
    {
        return std::make_unique<SetRow>(row_, before.arrangement.sections[at(row_)], label_, key_);
    }
    bool mergeWith(const Command& other, double) override
    {
        // The stack holds inverses: keeping this one (the state before the first turn) is the merge (EDT-U2).
        const auto* o = dynamic_cast<const SetRow*>(&other);
        return o != nullptr && ! key_.empty() && o->key_ == key_ && o->row_ == row_;
    }
    void collectChanges(ChangeSet& c) const override { c.add(std::string(kSectionsPath)); }
    std::size_t sizeBytes() const override { return sizeof(*this) + rowBytes(section_) + key_.capacity(); }

private:
    int row_;
    Section section_;
    std::string_view label_;
    std::string key_;
};

class MoveRow final : public Command
{
public:
    MoveRow(int from, int to) : from_(from), to_(to) {}
    std::string_view label() const override { return "MOVE ROW"; }
    void apply(Project& p) override
    {
        auto& v = p.arrangement.sections;
        if (at(from_) >= v.size() || at(to_) >= v.size())
            return;
        Section s = std::move(v[at(from_)]);
        v.erase(v.begin() + static_cast<std::ptrdiff_t>(from_));
        v.insert(v.begin() + static_cast<std::ptrdiff_t>(to_), std::move(s));
    }
    std::unique_ptr<Command> inverse(const Project&) const override { return std::make_unique<MoveRow>(to_, from_); }
    void collectChanges(ChangeSet& c) const override { c.add(std::string(kSectionsPath)); }

private:
    int from_, to_;
};

class ReplaceRows final : public Command
{
public:
    ReplaceRows(std::vector<Section> rows, std::string_view label) : rows_(std::move(rows)), label_(label) {}
    std::string_view label() const override { return label_; }
    void apply(Project& p) override
    {
        p.arrangement.sections = rows_;
        syncLength(p.arrangement);
    }
    std::unique_ptr<Command> inverse(const Project& before) const override
    {
        return std::make_unique<ReplaceRows>(before.arrangement.sections, label_);
    }
    void collectChanges(ChangeSet& c) const override { c.add(std::string(kSectionsPath)); }
    std::size_t sizeBytes() const override
    {
        std::size_t n = sizeof(*this);
        for (const auto& s : rows_)
            n += rowBytes(s);
        return n;
    }

private:
    std::vector<Section> rows_;
    std::string_view label_;
};

class SetPlay final : public Command
{
public:
    explicit SetPlay(bool on) : on_(on) {}
    std::string_view label() const override { return "ARRANGEMENT"; }
    void apply(Project& p) override { p.settings.playArrangement = on_; }
    std::unique_ptr<Command> inverse(const Project& before) const override
    {
        return std::make_unique<SetPlay>(before.settings.playArrangement);
    }
    void collectChanges(ChangeSet& c) const override { c.add(std::string(kPlayPath)); }

private:
    bool on_;
};

bool validRow(const Project& p, int row) noexcept
{
    return row >= 0 && row < rowCount(p);
}

double roundTenth(double v) noexcept
{
    return std::round(v * 10.0) / 10.0;
}

std::string fieldKey(int row, RowField f)
{
    return "row[" + std::to_string(row) + "]." + std::to_string(static_cast<int>(f));
}

std::string_view fieldLabel(RowField f) noexcept
{
    switch (f)
    {
        case RowField::Scene: return "SCENE";
        case RowField::Pattern: return "PATTERN";
        case RowField::Repeat: return "REPEAT";
        case RowField::Bpm: return "BPM";
    }
    return "ROW";
}
} // namespace

int rowCount(const Project& p) noexcept
{
    return static_cast<int>(std::min<std::size_t>(p.arrangement.sections.size(), static_cast<std::size_t>(kMaxRows)));
}

Section normalised(Section s) noexcept
{
    s.scene = static_cast<std::uint8_t>(s.scene & 15);
    s.pattern = static_cast<std::uint8_t>(s.pattern & 15);
    s.repeats = std::clamp(s.repeats, 1, kMaxRepeats);
    if (s.fixedBpm && ! (*s.fixedBpm >= model::kMinTempo))
        s.fixedBpm.reset(); // below 20 (or NaN) = "--"
    else if (s.fixedBpm)
        s.fixedBpm = roundTenth(std::min(*s.fixedBpm, model::kMaxTempo));
    return s;
}

Section currentRow(const Project& p)
{
    Section s;
    s.scene = static_cast<std::uint8_t>(p.settings.currentScene & 15);
    s.pattern = static_cast<std::uint8_t>(p.settings.currentPattern & 15);
    s.repeats = 1;
    return s;
}

float rowValue(const Project& p, int row, RowField f) noexcept
{
    const bool valid = validRow(p, row);
    const Section* s = valid ? &p.arrangement.sections[at(row)] : nullptr;
    switch (f)
    {
        case RowField::Scene: return s != nullptr ? static_cast<float>(s->scene & 15) : 0.0f;
        case RowField::Pattern: return s != nullptr ? static_cast<float>(s->pattern & 15) : 0.0f;
        case RowField::Repeat: return s != nullptr ? static_cast<float>(std::clamp(s->repeats, 1, kMaxRepeats)) : 1.0f;
        case RowField::Bpm: return s != nullptr && s->fixedBpm ? static_cast<float>(*s->fixedBpm) : 0.0f;
    }
    return 0.0f;
}

std::unique_ptr<Command> insertRowBefore(const Project& p, int row, Section s)
{
    if (row < 0 || row > rowCount(p) || rowCount(p) >= kMaxRows)
        return nullptr;
    return std::make_unique<InsertRow>(row, normalised(std::move(s)), "INSERT ROW");
}

std::unique_ptr<Command> insertCurrentBefore(const Project& p, int row)
{
    return insertRowBefore(p, row, currentRow(p));
}

std::unique_ptr<Command> appendRow(const Project& p, Section s)
{
    if (rowCount(p) >= kMaxRows)
        return nullptr;
    return std::make_unique<InsertRow>(rowCount(p), normalised(std::move(s)), "APPEND ROW");
}

std::unique_ptr<Command> appendCurrent(const Project& p)
{
    return appendRow(p, currentRow(p));
}

std::unique_ptr<Command> editRow(const Project& p, int row, Section s)
{
    if (! validRow(p, row))
        return nullptr;
    s = normalised(std::move(s));
    const Section& old = p.arrangement.sections[at(row)];
    if (s.scene == old.scene && s.pattern == old.pattern && s.repeats == old.repeats && s.fixedBpm == old.fixedBpm)
        return nullptr;
    return std::make_unique<SetRow>(row, std::move(s), "EDIT ROW", std::string());
}

std::unique_ptr<Command> editRowField(const Project& p, int row, RowField f, double value)
{
    if (! validRow(p, row) || ! (value == value))
        return nullptr;
    Section s = p.arrangement.sections[at(row)];
    const int i = static_cast<int>(std::lround(value));
    switch (f)
    {
        case RowField::Scene: s.scene = static_cast<std::uint8_t>(std::clamp(i, 0, 15)); break;
        case RowField::Pattern: s.pattern = static_cast<std::uint8_t>(std::clamp(i, 0, 15)); break;
        case RowField::Repeat: s.repeats = std::clamp(i, 1, kMaxRepeats); break;
        case RowField::Bpm:
            if (value < model::kMinTempo)
                s.fixedBpm.reset();
            else
                s.fixedBpm = value;
            break;
    }
    s = normalised(std::move(s));
    const Section& old = p.arrangement.sections[at(row)];
    if (s.scene == old.scene && s.pattern == old.pattern && s.repeats == old.repeats && s.fixedBpm == old.fixedBpm)
        return nullptr;
    return std::make_unique<SetRow>(row, std::move(s), fieldLabel(f), fieldKey(row, f));
}

std::unique_ptr<Command> deleteRow(const Project& p, int row)
{
    if (! validRow(p, row))
        return nullptr;
    return std::make_unique<DeleteRow>(row, "DELETE ROW");
}

std::unique_ptr<Command> moveRow(const Project& p, int from, int to)
{
    if (! validRow(p, from) || ! validRow(p, to) || from == to)
        return nullptr;
    return std::make_unique<MoveRow>(from, to);
}

std::unique_ptr<Command> duplicateRow(const Project& p, int row)
{
    if (! validRow(p, row) || rowCount(p) >= kMaxRows)
        return nullptr;
    return std::make_unique<InsertRow>(row + 1, p.arrangement.sections[at(row)], "DUPLICATE ROW"); // the copy after it
}

std::unique_ptr<Command> clearArrangement(const Project& p)
{
    if (p.arrangement.sections.empty())
        return nullptr;
    return std::make_unique<ReplaceRows>(std::vector<Section> {}, "CLEAR ARRANGEMENT");
}

std::unique_ptr<Command> setPlayArrangement(const Project& p, bool on)
{
    if (p.settings.playArrangement == on)
        return nullptr;
    return std::make_unique<SetPlay>(on);
}

bool RowClipboard::copy(const Project& p, int row)
{
    if (! validRow(p, row))
        return false;
    row_ = p.arrangement.sections[at(row)];
    return true;
}

std::unique_ptr<Command> RowClipboard::pasteBefore(const Project& p, int row) const
{
    if (! row_ || row < 0 || row > rowCount(p) || rowCount(p) >= kMaxRows)
        return nullptr;
    return std::make_unique<InsertRow>(row, normalised(*row_), "PASTE ROW");
}
} // namespace core::commands::arrangement
