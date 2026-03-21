// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Axertin

#include <windows.h>

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved)
{
    (void)hModule;
    (void)reserved;

    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(hModule);

    return TRUE;
}
