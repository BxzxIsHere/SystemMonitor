#pragma once

#include <cstdint>
#include <string>

namespace sysmon::ui {

// Presentation-only helpers. Everything below the UI works in raw bytes,
// seconds and 0..1 loads; this is the single place those turn into words.

// Binary units, because that is what every other tool on Windows reports.
std::wstring FormatBytes(std::uint64_t bytes);
std::wstring FormatBytesShort(std::uint64_t bytes);
std::wstring FormatRate(double bytesPerSecond);
std::wstring FormatPercent(float normalised, int decimals = 0);
std::wstring FormatFrequency(unsigned megahertz);
std::wstring FormatUptime(std::uint64_t seconds);
std::wstring FormatCount(std::uint64_t value);
std::wstring FormatLinkSpeed(std::uint64_t bitsPerSecond);

} // namespace sysmon::ui
