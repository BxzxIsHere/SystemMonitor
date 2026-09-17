#include "Logic/SystemInfo.h"

#include "Libs/Platform.h"

#include <intrin.h>

#include <cstring>
#include <vector>

namespace sysmon {
namespace {

// Windows 11 kept reporting itself as Windows 10 in the registry product name,
// so the build number is the only reliable way to tell them apart.
constexpr unsigned kFirstWindows11Build = 22000;

std::wstring Trim(std::wstring text) {
    const auto first = text.find_first_not_of(L" \t");
    if (first == std::wstring::npos) return {};
    const auto last = text.find_last_not_of(L" \t");
    return text.substr(first, last - first + 1);
}

std::wstring ReadRegistryString(const wchar_t* subKey, const wchar_t* value) {
    wchar_t buffer[256]{};
    DWORD size = sizeof(buffer);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, subKey, value, RRF_RT_REG_SZ, nullptr, buffer,
                     &size) != ERROR_SUCCESS) {
        return {};
    }
    return buffer;
}

std::wstring ProcessorBrand() {
    // Leaves 0x80000002 through 0x80000004 spell out the marketing name in
    // plain ASCII, four registers at a time.
    int registers[4]{};
    __cpuid(registers, 0x80000000);
    if (static_cast<unsigned>(registers[0]) < 0x80000004u) return L"Unknown processor";

    char brand[49]{};
    for (unsigned leaf = 0; leaf < 3; ++leaf) {
        __cpuid(registers, static_cast<int>(0x80000002u + leaf));
        std::memcpy(brand + leaf * sizeof(registers), registers, sizeof(registers));
    }

    std::wstring wide;
    wide.reserve(48);
    for (char character : brand) {
        if (character == '\0') break;
        wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(character)));
    }
    return Trim(std::move(wide));
}

unsigned PhysicalCoreCount() {
    DWORD size = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &size);
    if (size == 0) return 0;

    std::vector<std::uint8_t> buffer(size);
    if (!GetLogicalProcessorInformationEx(
            RelationProcessorCore,
            reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data()), &size)) {
        return 0;
    }

    unsigned cores = 0;
    for (DWORD offset = 0; offset < size;) {
        const auto* record =
            reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() +
                                                                             offset);
        if (record->Relationship == RelationProcessorCore) ++cores;
        offset += record->Size;
    }
    return cores;
}

SystemInfo Gather() {
    SystemInfo info;

    info.cpuBrand = ProcessorBrand();
    info.logicalCores = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    info.physicalCores = PhysicalCoreCount();
    if (info.physicalCores == 0) info.physicalCores = info.logicalCores;

    constexpr const wchar_t* kVersionKey = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    const unsigned build = slick::WindowsBuildNumber();

    std::wstring product = ReadRegistryString(kVersionKey, L"ProductName");
    if (product.empty()) product = L"Windows";
    if (build >= kFirstWindows11Build) {
        const auto ten = product.find(L"Windows 10");
        if (ten != std::wstring::npos) product.replace(ten, 10, L"Windows 11");
    }
    info.osName = product;

    const std::wstring release = ReadRegistryString(kVersionKey, L"DisplayVersion");
    info.osBuild = release.empty() ? std::to_wstring(build)
                                   : release + L" · build " + std::to_wstring(build);

    wchar_t name[256]{};
    DWORD length = ARRAYSIZE(name);
    if (GetComputerNameExW(ComputerNameDnsHostname, name, &length)) info.machineName = name;

    length = ARRAYSIZE(name);
    if (GetUserNameW(name, &length)) info.userName = name;

    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) info.totalMemoryBytes = memory.ullTotalPhys;

    return info;
}

} // namespace

const SystemInfo& SystemInfo::Current() {
    static const SystemInfo info = Gather();
    return info;
}

} // namespace sysmon
