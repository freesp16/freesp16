// TextField (components/controls.md "TextField", screens/project.md "SAVE AS"): 768 × 56 inset with a 1 lpx
// border-strong edge (accent and 2 lpx when focused; rec when invalid), body text at x + 16, a 2 lpx accent caret
// blinking 1 Hz (blink-slow). When the whole text is selected (dialog open, UID-905 "existing text selected"), typing
// replaces it. Holds its own UTF-8 text in a fixed buffer (64 characters max, PRJ-004), so editing never allocates.
#pragma once

#include "ui/widgets/Widget.h"

#include <cstddef>
#include <string_view>

namespace ui
{
class TextField final : public Widget
{
public:
    static constexpr std::size_t kMaxBytes = 256; // 64 characters of up to 4 bytes

    explicit TextField(RectF r = {}) noexcept : rect_(r) {}
    RectF bounds() const noexcept override { return rect_; }
    void build(DrawList&, const UiContext&) const override;
    bool dirty(double) const override { return focused; } // caret blink

    void setText(std::string_view s, bool selectAll) noexcept;
    std::string_view text() const noexcept { return {buf_, len_}; }
    std::size_t caret() const noexcept { return caret_; }
    bool allSelected() const noexcept { return selectAll_; }
    void insert(std::string_view utf8) noexcept; // replaces the selection
    void backspace() noexcept;
    void moveCaret(int chars) noexcept; // ± characters, clears the selection
    void moveWord(int words) noexcept;  // SHIFT + turn: caret by word
    std::size_t characters() const noexcept;

    bool focused = true;
    bool invalid = false;
    double caretSinceMs = 0; // the caret is solid right after a key, then blinks
    std::string_view placeholder{};

private:
    RectF rect_;
    char buf_[kMaxBytes]{};
    std::size_t len_ = 0, caret_ = 0;
    bool selectAll_ = false;
};
} // namespace ui
