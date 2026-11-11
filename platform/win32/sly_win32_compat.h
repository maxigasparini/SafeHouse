#pragma once
// Force-included (-include) into every C++ file of Sly2-Release / Sly3-Release on Windows only (top-level
// CMakeLists.txt). It supplies the few POSIX / glibc calls the ports use, so their sources build unchanged; Linux builds
// never see this file.
#ifdef __cplusplus
// <windows.h> first (WIN32_LEAN_AND_MEAN, NOMINMAX from CMake), then drop its macros that collide with names in the
// game sources (local variables "near" / "far", CHud::DrawText, ...). Later includes of <windows.h> are no-ops.
#include <windows.h>
// The DXGI source also needs the COM interfaces. Parse them before removing
// the SDK macros that collide with the Sly game source identifiers.
#if defined(CANE_DXGI_COM_HEADERS)
#include <objbase.h>
#endif
#undef near
#undef far
#undef DrawText
#undef GetObject
#undef PlaySound
#undef CreateWindow
#undef LoadImage
#undef SendMessage
#undef GetMessage
#undef CreateFont
#undef DrawState
#undef ERROR
#undef IGNORE
#undef ABSOLUTE
#undef RELATIVE
#undef TRANSPARENT
#undef OPAQUE
#undef DELETE
#undef interface
#include <cstddef>
#include <cstdlib>
// glibc's malloc_trim returns freed heap pages to the OS; the Windows heap does that by itself.
inline int malloc_trim(std::size_t) { return 0; }
inline int setenv(const char* name, const char* value, int overwrite) {
    if (!overwrite && std::getenv(name)) return 0;
    return _putenv_s(name, value);
}
inline int unsetenv(const char* name) { return _putenv_s(name, ""); }
#endif
