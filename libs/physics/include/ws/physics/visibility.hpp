#pragma once

// Export control for ws::physics. The lib builds STATIC today, where the macro
// is inert on MSVC; the branches exist for the day it builds shared.
// - MSVC: dllexport only while building the physics DLL (WS_PHYSICS_EXPORT),
//   dllimport while consuming it (WS_PHYSICS_IMPORT). A static consumer needs
//   neither, and modern MSVC DLLs export nothing without __declspec -
//   which is what makes this explicit.
// - GCC/Clang: the macro grants default visibility; the lib's CMakeLists adds
//   -fvisibility=hidden, so WS_PHYSICS_API is the complete export list.
#if defined(_MSC_VER)
#  if defined(WS_PHYSICS_EXPORT)
#    define WS_PHYSICS_API __declspec(dllexport)
#  elif defined(WS_PHYSICS_IMPORT)
#    define WS_PHYSICS_API __declspec(dllimport)
#  else
#    define WS_PHYSICS_API
#  endif
#else
#  define WS_PHYSICS_API __attribute__((visibility("default")))
#endif
