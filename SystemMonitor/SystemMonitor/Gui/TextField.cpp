#include "Gui/TextField.h"

#include "Libs/Clipboard.h"

#include <algorithm>
#include <cmath>

namespace sysmon::ui {
namespace {

using slick::Canvas;
using slick::Color;
using slick::HAlign;
using slick::Point;
using slick::Rect;
using slick::TextStyle;
using slick::Theme;
using slick::VAlign;

bool IsWordCharacter(wchar_t character) {
    return !(character == L' ' || character == L'\\' || character == L'/' || character == L'.' ||
             character == L'-' || character == L'_');
}

} // namespace

slick::TextStyle TextField::Style(const Theme& theme, float scale) const {
    return TextStyle{theme.fontText, 16.0f * scale, 400};
}

float TextField::Measure(Canvas& canvas, const Theme& theme, float scale,
                         std::size_t characters) const {
    if (characters == 0) return 0.0f;
    return canvas.MeasureString(std::wstring_view(text_).substr(0, characters),
                                Style(theme, scale))
        .width;
}

void TextField::Record(bool coalesce) {
    // A run of typed characters is one undo step. Anything else — a paste, a
    // delete, a clear — starts a new one.
    if (coalesce && lastEditWasTyping_ && !undo_.empty()) {
        lastEditWasTyping_ = true;
        return;
    }

    undo_.push_back(Snapshot{text_, caret_});
    redo_.clear();
    lastEditWasTyping_ = coalesce;

    // Deep enough to cover any editing anyone does in a filter box, shallow
    // enough that it cannot grow without limit over a long session.
    constexpr std::size_t kDepth = 64;
    if (undo_.size() > kDepth) undo_.erase(undo_.begin());
}

bool TextField::Restore(std::vector<Snapshot>& from, std::vector<Snapshot>& to) {
    if (from.empty()) return false;

    to.push_back(Snapshot{text_, caret_});
    text_ = std::move(from.back().text);
    caret_ = std::min(from.back().caret, text_.size());
    anchor_ = caret_;
    from.pop_back();

    // Whatever comes next starts a fresh step rather than merging into the one
    // that was just undone.
    lastEditWasTyping_ = false;
    return true;
}

void TextField::SetText(std::wstring text) {
    Record(false);
    text_ = std::move(text);
    caret_ = text_.size();
    anchor_ = caret_;
}

void TextField::SelectAll() {
    anchor_ = 0;
    caret_ = text_.size();
}

bool TextField::DeleteSelection() {
    if (!HasSelection()) return false;
    const std::size_t start = SelectionStart();
    text_.erase(start, SelectionEnd() - start);
    caret_ = start;
    anchor_ = start;
    return true;
}

bool TextField::Insert(wchar_t character) {
    // Control characters arrive here alongside real input; the ones that mean
    // something are handled as keys instead.
    if (character < 0x20 || character == 0x7F) return false;

    Record(!HasSelection());
    DeleteSelection();
    text_.insert(caret_, 1, character);
    ++caret_;
    anchor_ = caret_;
    return true;
}

std::size_t TextField::WordLeft() const {
    std::size_t at = caret_;
    while (at > 0 && !IsWordCharacter(text_[at - 1])) --at;
    while (at > 0 && IsWordCharacter(text_[at - 1])) --at;
    return at;
}

std::size_t TextField::WordRight() const {
    std::size_t at = caret_;
    while (at < text_.size() && IsWordCharacter(text_[at])) ++at;
    while (at < text_.size() && !IsWordCharacter(text_[at])) ++at;
    return at;
}

bool TextField::HandleKey(int virtualKey, bool control, bool shift, HWND owner) {
    const std::size_t before = text_.size();
    auto moved = [&](std::size_t position) {
        caret_ = position;
        if (!shift) anchor_ = caret_;
    };

    switch (virtualKey) {
        case VK_LEFT:
            moved(control ? WordLeft() : (caret_ > 0 ? caret_ - 1 : 0));
            return false;

        case VK_RIGHT:
            moved(control ? WordRight() : std::min(caret_ + 1, text_.size()));
            return false;

        case VK_HOME:
            moved(0);
            return false;

        case VK_END:
            moved(text_.size());
            return false;

        case VK_BACK:
            Record(false);
            if (!DeleteSelection()) {
                const std::size_t from = control ? WordLeft() : (caret_ > 0 ? caret_ - 1 : 0);
                if (from != caret_) {
                    text_.erase(from, caret_ - from);
                    caret_ = from;
                    anchor_ = from;
                }
            }
            return text_.size() != before;

        case VK_DELETE:
            Record(false);
            if (!DeleteSelection() && caret_ < text_.size()) {
                const std::size_t to = control ? WordRight() : caret_ + 1;
                text_.erase(caret_, to - caret_);
            }
            return text_.size() != before;

        case 'A':
            if (control) SelectAll();
            return false;

        case 'C':
        case 'X':
            if (control && HasSelection()) {
                slick::WriteClipboardText(
                    owner, std::wstring_view(text_).substr(SelectionStart(),
                                                           SelectionEnd() - SelectionStart()));
                if (virtualKey == 'X') {
                    Record(false);
                    return DeleteSelection();
                }
            }
            return false;

        case 'V': {
            if (!control) return false;
            std::wstring pasted = slick::ReadClipboardText(owner);
            // A pasted path arrives with its line ending attached more often
            // than not, and a newline in a search box is never wanted.
            pasted.erase(std::remove_if(pasted.begin(), pasted.end(),
                                        [](wchar_t c) { return c == L'\r' || c == L'\n'; }),
                         pasted.end());
            if (pasted.empty()) return false;

            Record(false);
            DeleteSelection();
            text_.insert(caret_, pasted);
            caret_ += pasted.size();
            anchor_ = caret_;
            return true;
        }

        case 'Z':
            // Ctrl+Z undoes, Ctrl+Shift+Z redoes, which is the other half of
            // the convention Ctrl+Y covers.
            if (!control) return false;
            return shift ? Restore(redo_, undo_) : Restore(undo_, redo_);

        case 'Y':
            if (!control) return false;
            return Restore(redo_, undo_);

        default:
            return false;
    }
}

void TextField::PlaceCaret(Canvas& canvas, const Rect& box, Point position, const Theme& theme,
                           float scale) {
    const float target = position.x - box.left + scroll_;

    // Walked rather than binary searched: a query is a few dozen characters at
    // most, and each step is a cached measurement.
    std::size_t best = 0;
    float bestDistance = std::abs(target);
    for (std::size_t at = 1; at <= text_.size(); ++at) {
        const float distance = std::abs(Measure(canvas, theme, scale, at) - target);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = at;
        }
    }

    caret_ = best;
    anchor_ = best;
}

void TextField::Draw(Canvas& canvas, const Rect& box, const Theme& theme, float scale,
                     double seconds, bool focused) {
    const TextStyle style = Style(theme, scale);

    if (text_.empty() && !placeholder_.empty()) {
        canvas.DrawString(placeholder_, style, box, theme.textFaint, HAlign::Left,
                          VAlign::Middle);
    }

    const float caretX = Measure(canvas, theme, scale, caret_);

    // Keep the caret inside the box, scrolling the text under it when the query
    // outgrows the space.
    const float visible = box.Width();
    if (caretX - scroll_ > visible - 2.0f * scale) scroll_ = caretX - visible + 2.0f * scale;
    if (caretX - scroll_ < 0.0f) scroll_ = caretX;
    const float fullWidth = Measure(canvas, theme, scale, text_.size());
    if (fullWidth - scroll_ < visible) scroll_ = std::max(0.0f, fullWidth - visible);

    canvas.PushClip(box);

    const Rect line{box.left - scroll_, box.top, box.left - scroll_ + 100000.0f, box.bottom};

    if (HasSelection()) {
        const float from = Measure(canvas, theme, scale, SelectionStart());
        const float to = Measure(canvas, theme, scale, SelectionEnd());
        canvas.FillRoundedRect(Rect{line.left + from, box.top + 2.0f * scale, line.left + to,
                                    box.bottom - 2.0f * scale},
                               2.0f * scale, theme.accent.WithAlpha(0.28f));
    }

    if (!text_.empty()) {
        canvas.DrawString(text_, style, line, theme.textPrimary, HAlign::Left, VAlign::Middle);
    }

    if (focused) {
        // A caret that blinks on a half second cycle, and holds steady while
        // the cursor is actually moving so it never disappears mid-edit.
        const double phase = std::fmod(seconds, 1.0);
        if (phase < 0.55) {
            const float x = canvas.Snap(line.left + caretX);
            canvas.FillRect(Rect{x, box.top + 2.0f * scale, x + canvas.Hairline() * 2.0f,
                                 box.bottom - 2.0f * scale},
                            theme.accent);
        }
    }

    canvas.PopClip();
}

} // namespace sysmon::ui
