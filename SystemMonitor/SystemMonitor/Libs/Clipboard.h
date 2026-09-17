#pragma once

#include "Libs/Platform.h"

#include <string>
#include <string_view>

namespace slick {

// Unicode text in and out of the clipboard.
//
// Both quietly do nothing if the clipboard is busy: another process can hold it
// open, and failing to paste is never worth interrupting the user over.
std::wstring ReadClipboardText(HWND owner);
void WriteClipboardText(HWND owner, std::wstring_view text);

} // namespace slick
