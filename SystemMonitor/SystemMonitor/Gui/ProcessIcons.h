#pragma once

#include "Libs/Canvas.h"

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>

namespace sysmon::ui {

// The shell's own icon for each process in the table.
//
// Two caches, because the two costs are different. Resolving a process id to the
// file it is running needs a handle opened and a path queried, and that is done
// once per process id. Turning a file into an icon is the expensive half, and it
// is keyed by path, so ten Chrome processes share one icon rather than building
// ten identical ones.
//
// Nothing here ever blocks a frame: rows that have not been resolved yet draw
// the generic icon, and a small budget per frame fills the rest in over the next
// few frames.
class ProcessIcons {
public:
    // Never null once the fallback has been built, so a row always has something
    // to draw. Spends from the budget only when it has to build something new.
    ID2D1Bitmap* Acquire(slick::Canvas& canvas, std::uint32_t pid, int& budget);

    // Icons belong to the device that made them, so a lost device drops them.
    void Clear();

private:
    // Null while the budget is spent, and empty when the process could not be
    // opened, which is normal for anything running as the system.
    const std::wstring* PathFor(std::uint32_t pid, int& budget);
    ID2D1Bitmap* Fallback(slick::Canvas& canvas);

    std::unordered_map<std::uint32_t, std::wstring> paths_;
    std::map<std::wstring, slick::ComPtr<ID2D1Bitmap>> icons_;
    slick::ComPtr<ID2D1Bitmap> fallback_;
    bool fallbackTried_ = false;
};

} // namespace sysmon::ui
