#pragma once

// Single place where <windows.h> enters the build, so every translation unit
// agrees on the same trimmed-down, Unicode, min/max-free view of it.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef OEMRESOURCE
#define OEMRESOURCE
#endif

// Windows 10 is the floor. Several headers gate their modern half behind this,
// notably the netioapi interface tables and the newer DWM frame attributes.
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>

namespace slick {

// The real build number, unfiltered by the compatibility shim that GetVersionEx
// is subject to. Queried once; it cannot change while the process runs.
unsigned WindowsBuildNumber();

} // namespace slick
