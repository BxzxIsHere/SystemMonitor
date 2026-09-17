#include "Libs/Platform.h"

namespace slick {

unsigned WindowsBuildNumber() {
    static const unsigned build = [] {
        using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOEXW*);

        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        const auto entry =
            ntdll ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"))
                  : nullptr;
        if (!entry) return 0u;

        OSVERSIONINFOEXW version{};
        version.dwOSVersionInfoSize = sizeof(version);
        if (entry(&version) < 0) return 0u;
        return static_cast<unsigned>(version.dwBuildNumber);
    }();
    return build;
}

} // namespace slick
