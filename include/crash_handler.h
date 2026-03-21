// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Axertin

#pragma once

#include <windows.h>

#ifdef CRASHLOGGERPP_EXPORTS
#define CLPP_API __declspec(dllexport)
#else
#define CLPP_API __declspec(dllimport)
#endif

namespace crashloggerpp
{

void install(const wchar_t *crashDirectory = nullptr, bool enableMinidump = false);
void uninstall();

} // namespace crashloggerpp

extern "C"
{
CLPP_API void clpp_install(const wchar_t *crashDirectory, int enableMinidump);
CLPP_API void clpp_uninstall();
}
