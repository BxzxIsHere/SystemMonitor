#include "Gui/Format.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace sysmon::ui {
namespace {

constexpr double kKibi = 1024.0;

std::wstring Printf(const wchar_t* format, ...) {
    wchar_t buffer[128]{};
    va_list arguments;
    va_start(arguments, format);
    vswprintf_s(buffer, format, arguments);
    va_end(arguments);
    return buffer;
}

// Below ten the first decimal carries real information; above it the digit is
// just noise, so the unit switches to whole numbers. Same rule every panel.
std::wstring Scale(double value, const wchar_t* const* units, std::size_t unitCount,
                   const wchar_t* suffix) {
    std::size_t unit = 0;
    while (value >= kKibi && unit + 1 < unitCount) {
        value /= kKibi;
        ++unit;
    }

    // Bytes are always whole; above that the digits earn their place only while
    // the number is small.
    const wchar_t* format = L"%.0f %s%s";
    if (unit > 0 && value < 10.0) format = L"%.2f %s%s";
    else if (unit > 0 && value < 100.0) format = L"%.1f %s%s";

    return Printf(format, value, units[unit], suffix);
}

constexpr const wchar_t* kByteUnits[] = {L"B", L"KB", L"MB", L"GB", L"TB", L"PB"};

} // namespace

std::wstring FormatBytes(std::uint64_t bytes) {
    return Scale(static_cast<double>(bytes), kByteUnits, std::size(kByteUnits), L"");
}

std::wstring FormatBytesShort(std::uint64_t bytes) {
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= kKibi && unit + 1 < std::size(kByteUnits)) {
        value /= kKibi;
        ++unit;
    }
    return Printf(value < 10.0 ? L"%.1f%s" : L"%.0f%s", value, kByteUnits[unit]);
}

std::wstring FormatRate(double bytesPerSecond) {
    if (bytesPerSecond < 1.0) return L"0 B/s";
    return Scale(bytesPerSecond, kByteUnits, std::size(kByteUnits), L"/s");
}

std::wstring FormatPercent(float normalised, int decimals) {
    const double percent = std::clamp(normalised, 0.0f, 1.0f) * 100.0;
    return Printf(decimals == 1 ? L"%.1f%%" : L"%.0f%%", percent);
}

std::wstring FormatFrequency(unsigned megahertz) {
    if (megahertz == 0) return L"--";
    if (megahertz < 1000) return Printf(L"%u MHz", megahertz);
    return Printf(L"%.2f GHz", megahertz / 1000.0);
}

std::wstring FormatUptime(std::uint64_t seconds) {
    const std::uint64_t days = seconds / 86400;
    const std::uint64_t hours = (seconds % 86400) / 3600;
    const std::uint64_t minutes = (seconds % 3600) / 60;

    if (days > 0) return Printf(L"%llud %lluh %llum", days, hours, minutes);
    if (hours > 0) return Printf(L"%lluh %llum", hours, minutes);
    return Printf(L"%llum %llus", minutes, seconds % 60);
}

std::wstring FormatCount(std::uint64_t value) {
    std::wstring digits = std::to_wstring(value);
    // Grouped by hand rather than through a locale, so the readout looks the
    // same on every machine this ends up running on.
    for (std::size_t position = digits.size(); position > 3;) {
        position -= 3;
        digits.insert(position, 1, L',');
    }
    return digits;
}

std::wstring FormatLinkSpeed(std::uint64_t bitsPerSecond) {
    if (bitsPerSecond == 0) return L"--";
    const double megabits = bitsPerSecond / 1'000'000.0;
    if (megabits >= 1000.0) return Printf(L"%.0f Gbps", megabits / 1000.0);
    return Printf(L"%.0f Mbps", megabits);
}

} // namespace sysmon::ui
