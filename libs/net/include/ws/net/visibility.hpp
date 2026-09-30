#pragma once

// Export control for ws::net. The lib builds STATIC today, where the macro
// is inert on MSVC; the branches exist for the day it builds shared.
// - MSVC: dllexport only while building the net DLL (WS_NET_EXPORT),
//   dllimport while consuming it (WS_NET_IMPORT). A static consumer needs
//   neither, and modern MSVC DLLs export nothing without __declspec -
//   which is what makes this explicit.
// - GCC/Clang: the macro grants default visibility; libs/net/CMakeLists.txt
//   adds -fvisibility=hidden, so WS_NET_API is the complete export list.
#if defined(_MSC_VER)
#  if defined(WS_NET_EXPORT)
#    define WS_NET_API __declspec(dllexport)
#  elif defined(WS_NET_IMPORT)
#    define WS_NET_API __declspec(dllimport)
#  else
#    define WS_NET_API
#  endif
#else
#  define WS_NET_API __attribute__((visibility("default")))
#endif
