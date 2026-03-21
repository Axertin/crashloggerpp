// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Axertin
//
// crashloggerpp.h — standalone consumer header
//
// Include this header in your project to load and interact with
// crashloggerpp at runtime. No link-time dependency required.

#pragma once

#include <windows.h>

namespace crashloggerpp {

using InstallFn = void (*)(const wchar_t *, int);
using UninstallFn = void (*)();

inline HMODULE g_Module = nullptr;
inline InstallFn g_Install = nullptr;
inline UninstallFn g_Uninstall = nullptr;

/// Loads the crashloggerpp DLL and resolves its exports.
/// @param dllPath  Path to the DLL (optional).
/// @return true if the DLL was loaded (or was already loaded), false on
/// failure.
inline bool load(const wchar_t *dllPath = L"crashloggerpp.dll") {
  if (g_Module)
    return true;

  g_Module = LoadLibraryW(dllPath);
  if (!g_Module)
    return false;

  g_Install =
      reinterpret_cast<InstallFn>(GetProcAddress(g_Module, "clpp_install"));
  g_Uninstall =
      reinterpret_cast<UninstallFn>(GetProcAddress(g_Module, "clpp_uninstall"));

  if (!g_Install || !g_Uninstall) {
    FreeLibrary(g_Module);
    g_Module = nullptr;
    g_Install = nullptr;
    g_Uninstall = nullptr;
    return false;
  }

  return true;
}

/// Uninstalls the crash handler (if active) and frees the DLL.
/// Safe to call even if install() was never called.
inline void uninstall() {
  if (g_Module) {
    g_Uninstall();
    FreeLibrary(g_Module);
    g_Module = nullptr;
  }

  g_Install = nullptr;
  g_Uninstall = nullptr;
}

/// Installs the vectored exception handler for crash logging.
/// Requires a prior call to load(). Multiple calls are safe — subsequent
/// calls are ignored while a handler is already registered.
/// @param crashDirectory  Absolute path for crash output, or nullptr to
///                        default to <cwd>\logs\crashes.
/// @param enableMinidump  Write a .dmp minidump alongside the crash log.
inline void install(const wchar_t *crashDirectory = nullptr,
                    bool enableMinidump = false) {
  if (g_Install)
    g_Install(crashDirectory, enableMinidump ? 1 : 0);
}

} // namespace crashloggerpp
