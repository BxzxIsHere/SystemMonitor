#pragma once

#include "Libs/Canvas.h"
#include "Libs/Theme.h"

#include <string>
#include <vector>

namespace sysmon::ui {

// A single line editor, enough for a search box and no more.
//
// The window has no child controls, so there is nothing else competing for the
// keyboard: every character the window receives belongs here.
class TextField {
public:
    const std::wstring& Text() const { return text_; }
    void SetText(std::wstring text);

    // Shown in place of the text while the field is empty. Drawn by the field
    // rather than the view, so it lands on the same baseline as real text.
    void SetPlaceholder(std::wstring text) { placeholder_ = std::move(text); }

    // True when the text changed, which is the signal to run a new search.
    bool Insert(wchar_t character);
    bool HandleKey(int virtualKey, bool control, bool shift, HWND owner);

    void SelectAll();
    bool Empty() const { return text_.empty(); }

    // Caret position and selection are laid out here too, so the box scrolls to
    // follow the caret when the query runs past its width.
    void Draw(slick::Canvas& canvas, const slick::Rect& box, const slick::Theme& theme,
              float scale, double seconds, bool focused);

    void PlaceCaret(slick::Canvas& canvas, const slick::Rect& box, slick::Point position,
                    const slick::Theme& theme, float scale);

private:
    // One point in the edit history: the text and where the caret was in it,
    // because an undo that leaves the caret somewhere else is disorienting.
    struct Snapshot {
        std::wstring text;
        std::size_t caret = 0;
    };

    slick::TextStyle Style(const slick::Theme& theme, float scale) const;
    float Measure(slick::Canvas& canvas, const slick::Theme& theme, float scale,
                  std::size_t characters) const;

    std::size_t SelectionStart() const { return caret_ < anchor_ ? caret_ : anchor_; }
    std::size_t SelectionEnd() const { return caret_ < anchor_ ? anchor_ : caret_; }
    bool HasSelection() const { return caret_ != anchor_; }
    bool DeleteSelection();

    std::size_t WordLeft() const;
    std::size_t WordRight() const;

    // Remembers the state before an edit, so it can be returned to. Typing a
    // run of characters collapses into one step: undoing a word at a time is
    // what every other text box on the machine does, and undoing a letter at a
    // time is infuriating.
    void Record(bool coalesce);
    bool Restore(std::vector<Snapshot>& from, std::vector<Snapshot>& to);

    std::vector<Snapshot> undo_;
    std::vector<Snapshot> redo_;
    bool lastEditWasTyping_ = false;

    std::wstring text_;
    std::wstring placeholder_;
    std::size_t caret_ = 0;
    std::size_t anchor_ = 0;
    float scroll_ = 0.0f;
};

} // namespace sysmon::ui
