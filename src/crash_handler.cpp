// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Axertin

#include "crash_handler.h"

#include <atomic>
#include <cstdio>

#include <psapi.h>

// DbgHelp types and function pointers (loaded dynamically)
using SymInitialize_t = BOOL(WINAPI *)(HANDLE, PCSTR, BOOL);
using SymFromAddr_t = BOOL(WINAPI *)(HANDLE, DWORD64, PDWORD64, void *);
using SymGetLineFromAddr64_t = BOOL(WINAPI *)(HANDLE, DWORD64, PDWORD, void *);
using SymCleanup_t = BOOL(WINAPI *)(HANDLE);
using MiniDumpWriteDump_t = BOOL(WINAPI *)(HANDLE, DWORD, HANDLE, DWORD, void *,
                                           void *, void *);

#if defined(_M_X64) || defined(__x86_64__)
// NT unwind functions (loaded from kernel32, x64 only)
using RtlLookupFunctionEntry_t = void *(WINAPI *)(DWORD64, PDWORD64, void *);
using RtlVirtualUnwind_t = void *(WINAPI *)(DWORD, DWORD64, DWORD64, void *,
                                            PCONTEXT, PVOID *, PDWORD64,
                                            void *);
#endif

// SYMBOL_INFO structure for DbgHelp (avoid including dbghelp.h which may not be
// available)
#pragma pack(push, 8)
struct CLPP_SYMBOL_INFO {
  ULONG SizeOfStruct;
  ULONG TypeIndex;
  ULONG64 Reserved[2];
  ULONG Index;
  ULONG Size;
  ULONG64 ModBase;
  ULONG Flags;
  ULONG64 Value;
  ULONG64 Address;
  ULONG Register;
  ULONG Scope;
  ULONG Tag;
  ULONG NameLen;
  ULONG MaxNameLen;
  CHAR Name[256];
};

struct CLPP_IMAGEHLP_LINE64 {
  DWORD SizeOfStruct;
  PVOID Key;
  DWORD LineNumber;
  PCHAR FileName;
  DWORD64 Address;
};

struct CLPP_MINIDUMP_EXCEPTION_INFORMATION {
  DWORD ThreadId;
  PEXCEPTION_POINTERS ExceptionPointers;
  BOOL ClientPointers;
};
#pragma pack(pop)

// MiniDumpWithThreadInfo = 0x00001000
static constexpr DWORD CLPP_MiniDumpWithThreadInfo = 0x00001000;

// Static crash-safe resources (pre-allocated, no runtime allocation)
static char g_CrashBuffer[65536];
static wchar_t g_CrashDirPath[MAX_PATH];
static PVOID g_VehHandle = nullptr;
static std::atomic<LONG> g_CrashHandlerActive{0};
static bool g_MinidumpEnabled = false;

// DbgHelp function pointers (symbol resolution and minidumps only)
static HMODULE g_DbgHelp = nullptr;
static SymInitialize_t g_SymInitialize = nullptr;
static SymFromAddr_t g_SymFromAddr = nullptr;
static SymGetLineFromAddr64_t g_SymGetLineFromAddr64 = nullptr;
static SymCleanup_t g_SymCleanup = nullptr;
static MiniDumpWriteDump_t g_MiniDumpWriteDump = nullptr;
static bool g_SymInitialized = false;

#if defined(_M_X64) || defined(__x86_64__)
// NT unwind function pointers (stack walking, x64 only)
static RtlLookupFunctionEntry_t g_RtlLookupFunctionEntry = nullptr;
static RtlVirtualUnwind_t g_RtlVirtualUnwind = nullptr;
#endif

static bool isFatalException(DWORD code) {
  switch (code) {
  case EXCEPTION_ACCESS_VIOLATION:
  case EXCEPTION_STACK_OVERFLOW:
  case EXCEPTION_INT_DIVIDE_BY_ZERO:
  case EXCEPTION_ILLEGAL_INSTRUCTION:
  case EXCEPTION_PRIV_INSTRUCTION:
  case EXCEPTION_IN_PAGE_ERROR:
  case EXCEPTION_GUARD_PAGE:
    return true;
  default:
    return false;
  }
}

static const char *exceptionCodeToString(DWORD code) {
  switch (code) {
  case EXCEPTION_ACCESS_VIOLATION:
    return "EXCEPTION_ACCESS_VIOLATION";
  case EXCEPTION_STACK_OVERFLOW:
    return "EXCEPTION_STACK_OVERFLOW";
  case EXCEPTION_INT_DIVIDE_BY_ZERO:
    return "EXCEPTION_INT_DIVIDE_BY_ZERO";
  case EXCEPTION_ILLEGAL_INSTRUCTION:
    return "EXCEPTION_ILLEGAL_INSTRUCTION";
  case EXCEPTION_PRIV_INSTRUCTION:
    return "EXCEPTION_PRIV_INSTRUCTION";
  case EXCEPTION_IN_PAGE_ERROR:
    return "EXCEPTION_IN_PAGE_ERROR";
  case EXCEPTION_GUARD_PAGE:
    return "EXCEPTION_GUARD_PAGE";
  default:
    return "UNKNOWN_EXCEPTION";
  }
}

// Crash-safe snprintf append helper (returns new offset)
static int crashAppend(char *buf, int bufSize, int offset, const char *fmt,
                       ...) {
  if (offset >= bufSize - 1)
    return offset;
  va_list args;
  va_start(args, fmt);
  int written = vsnprintf(buf + offset, bufSize - offset, fmt, args);
  va_end(args);
  if (written > 0)
    offset += written;
  if (offset >= bufSize)
    offset = bufSize - 1;
  return offset;
}

static int resolveFrame(char *buf, int bufSize, int offset, HANDLE process,
                        int frameIndex, DWORD64 frameAddr) {
  HMODULE hModule = nullptr;
  char moduleName[256] = "???";
  DWORD64 moduleBase = 0;

  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(frameAddr), &hModule)) {
    char modulePathA[MAX_PATH];
    if (GetModuleFileNameA(hModule, modulePathA, MAX_PATH)) {
      const char *lastSlash = modulePathA;
      for (const char *p = modulePathA; *p; p++) {
        if (*p == '\\' || *p == '/')
          lastSlash = p + 1;
      }
      int j = 0;
      for (const char *p = lastSlash; *p && j < 255; p++, j++)
        moduleName[j] = *p;
      moduleName[j] = '\0';
    }
    moduleBase = reinterpret_cast<DWORD64>(hModule);
  }

  DWORD64 offsetFromBase = frameAddr - moduleBase;

  bool symbolResolved = false;
  if (g_SymFromAddr && g_SymInitialized) {
    CLPP_SYMBOL_INFO symInfo = {};
    symInfo.SizeOfStruct = sizeof(CLPP_SYMBOL_INFO) - sizeof(symInfo.Name) + 1;
    symInfo.MaxNameLen = 255;
    DWORD64 displacement = 0;

    if (g_SymFromAddr(process, frameAddr, &displacement, &symInfo)) {
      if (g_SymGetLineFromAddr64) {
        CLPP_IMAGEHLP_LINE64 lineInfo = {};
        lineInfo.SizeOfStruct = sizeof(CLPP_IMAGEHLP_LINE64);
        DWORD lineDisp = 0;
        if (g_SymGetLineFromAddr64(process, frameAddr, &lineDisp, &lineInfo)) {
          offset = crashAppend(buf, bufSize, offset,
                               "  #%-2d %s!%s+0x%llX (%s:%lu)\n", frameIndex,
                               moduleName, symInfo.Name, displacement,
                               lineInfo.FileName, lineInfo.LineNumber);
          symbolResolved = true;
        }
      }

      if (!symbolResolved) {
        offset =
            crashAppend(buf, bufSize, offset, "  #%-2d %s!%s+0x%llX\n",
                        frameIndex, moduleName, symInfo.Name, displacement);
        symbolResolved = true;
      }
    }
  }

  if (!symbolResolved) {
    offset = crashAppend(buf, bufSize, offset, "  #%-2d %s+0x%llX\n",
                         frameIndex, moduleName, offsetFromBase);
  }

  return offset;
}

static int formatStackTrace(char *buf, int bufSize, int offset, CONTEXT *ctx) {
  offset = crashAppend(buf, bufSize, offset, "\nStack trace:\n");

  HANDLE process = GetCurrentProcess();

#if defined(_M_X64) || defined(__x86_64__)
  // x64: Use RtlVirtualUnwind to walk the faulting thread's actual stack.
  // Unlike StackWalk64 (which depends on SymFunctionTableAccess64 from
  // dbghelp), RtlLookupFunctionEntry reads .pdata directly from loaded PE
  // modules — no PDB symbols needed.
  if (g_RtlLookupFunctionEntry && g_RtlVirtualUnwind) {
    CONTEXT ctxCopy = *ctx;

    for (int i = 0; i < 64; i++) {
      DWORD64 pc = ctxCopy.Rip;
      if (pc == 0)
        break;

      offset = resolveFrame(buf, bufSize, offset, process, i, pc);

      DWORD64 imageBase = 0;
      auto *funcEntry = g_RtlLookupFunctionEntry(pc, &imageBase, nullptr);

      if (funcEntry) {
        // Function has .pdata unwind info — use RtlVirtualUnwind
        PVOID handlerData = nullptr;
        DWORD64 establisherFrame = 0;
        g_RtlVirtualUnwind(0 /* UNW_FLAG_NHANDLER */, imageBase, pc, funcEntry,
                           &ctxCopy, &handlerData, &establisherFrame, nullptr);
      } else {
        // Leaf function (no .pdata entry) — return address is at [RSP]
        if (ctxCopy.Rsp == 0)
          break;
        ctxCopy.Rip = *reinterpret_cast<DWORD64 *>(ctxCopy.Rsp);
        ctxCopy.Rsp += 8;
      }
    }
  } else {
    // Fallback: CaptureStackBackTrace (captures handler stack, not fault stack)
    void *frames[64];
    USHORT frameCount = CaptureStackBackTrace(0, 64, frames, nullptr);

    for (USHORT i = 0; i < frameCount; i++) {
      offset = resolveFrame(buf, bufSize, offset, process, i,
                            reinterpret_cast<DWORD64>(frames[i]));
    }
  }
#else
  // x86: Walk the EBP frame chain from the exception context
  {
    DWORD eip = ctx->Eip;
    DWORD ebp = ctx->Ebp;
    MEMORY_BASIC_INFORMATION mbi = {};

    for (int i = 0; i < 64; i++) {
      if (eip == 0)
        break;

      offset = resolveFrame(buf, bufSize, offset, process, i,
                            static_cast<DWORD64>(eip));

      // Validate EBP points to readable memory before dereferencing
      if (ebp == 0 || (ebp & 3) != 0)
        break;
      if (!VirtualQuery(reinterpret_cast<void *>(ebp), &mbi, sizeof(mbi)))
        break;
      if (!(mbi.State == MEM_COMMIT &&
            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE |
                            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))))
        break;

      // Follow the EBP chain: [EBP+4] = return address, [EBP] = previous EBP
      eip = *reinterpret_cast<DWORD *>(ebp + 4);
      ebp = *reinterpret_cast<DWORD *>(ebp);
    }
  }
#endif

  return offset;
}

static int formatRegisters(char *buf, int bufSize, int offset, CONTEXT *ctx) {
  offset = crashAppend(buf, bufSize, offset, "\nRegisters:\n");

#if defined(_M_X64) || defined(__x86_64__)
  offset = crashAppend(buf, bufSize, offset, "  RAX=%016llX  RBX=%016llX\n",
                       ctx->Rax, ctx->Rbx);
  offset = crashAppend(buf, bufSize, offset, "  RCX=%016llX  RDX=%016llX\n",
                       ctx->Rcx, ctx->Rdx);
  offset = crashAppend(buf, bufSize, offset, "  RSP=%016llX  RBP=%016llX\n",
                       ctx->Rsp, ctx->Rbp);
  offset = crashAppend(buf, bufSize, offset, "  RSI=%016llX  RDI=%016llX\n",
                       ctx->Rsi, ctx->Rdi);
  offset = crashAppend(buf, bufSize, offset, "  RIP=%016llX  RFLAGS=%08lX\n",
                       ctx->Rip, ctx->EFlags);
  offset = crashAppend(buf, bufSize, offset, "  R8 =%016llX  R9 =%016llX\n",
                       ctx->R8, ctx->R9);
  offset = crashAppend(buf, bufSize, offset, "  R10=%016llX  R11=%016llX\n",
                       ctx->R10, ctx->R11);
  offset = crashAppend(buf, bufSize, offset, "  R12=%016llX  R13=%016llX\n",
                       ctx->R12, ctx->R13);
  offset = crashAppend(buf, bufSize, offset, "  R14=%016llX  R15=%016llX\n",
                       ctx->R14, ctx->R15);
#else
  offset = crashAppend(buf, bufSize, offset, "  EAX=%08lX  EBX=%08lX\n",
                       ctx->Eax, ctx->Ebx);
  offset = crashAppend(buf, bufSize, offset, "  ECX=%08lX  EDX=%08lX\n",
                       ctx->Ecx, ctx->Edx);
  offset = crashAppend(buf, bufSize, offset, "  ESP=%08lX  EBP=%08lX\n",
                       ctx->Esp, ctx->Ebp);
  offset = crashAppend(buf, bufSize, offset, "  ESI=%08lX  EDI=%08lX\n",
                       ctx->Esi, ctx->Edi);
  offset = crashAppend(buf, bufSize, offset, "  EIP=%08lX  EFLAGS=%08lX\n",
                       ctx->Eip, ctx->EFlags);
#endif

  return offset;
}

static int formatModuleList(char *buf, int bufSize, int offset) {
  offset = crashAppend(buf, bufSize, offset, "\nLoaded modules:\n");

  HANDLE process = GetCurrentProcess();
  HMODULE modules[256];
  DWORD cbNeeded = 0;

  if (EnumProcessModules(process, modules, sizeof(modules), &cbNeeded)) {
    DWORD moduleCount = cbNeeded / sizeof(HMODULE);
    if (moduleCount > 256)
      moduleCount = 256;

    for (DWORD i = 0; i < moduleCount; i++) {
      char moduleName[MAX_PATH] = {};
      MODULEINFO modInfo = {};

      GetModuleFileNameA(modules[i], moduleName, MAX_PATH);
      GetModuleInformation(process, modules[i], &modInfo, sizeof(modInfo));

      // Extract just filename
      const char *name = moduleName;
      for (const char *p = moduleName; *p; p++) {
        if (*p == '\\' || *p == '/')
          name = p + 1;
      }

      offset = crashAppend(buf, bufSize, offset, "  %-30s 0x%p  (%lu bytes)\n",
                           name, modInfo.lpBaseOfDll,
                           static_cast<unsigned long>(modInfo.SizeOfImage));
    }
  }

  return offset;
}

static void writeMinidump(EXCEPTION_POINTERS *exInfo) {
  if (!g_MinidumpEnabled)
    return;

  if (!g_MiniDumpWriteDump)
    return;

  SYSTEMTIME st;
  GetLocalTime(&st);
  wchar_t dumpPath[MAX_PATH];
  _snwprintf(dumpPath, MAX_PATH,
             L"%s\\crash_%04d%02d%02d_%02d%02d%02d_%03d_%lu.dmp",
             g_CrashDirPath, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
             st.wSecond, st.wMilliseconds, GetCurrentProcessId());

  HANDLE hFile = CreateFileW(dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
  if (hFile == INVALID_HANDLE_VALUE)
    return;

  CLPP_MINIDUMP_EXCEPTION_INFORMATION mei = {};
  mei.ThreadId = GetCurrentThreadId();
  mei.ExceptionPointers = exInfo;
  mei.ClientPointers = FALSE;

  g_MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile,
                      CLPP_MiniDumpWithThreadInfo, &mei, nullptr, nullptr);

  CloseHandle(hFile);
}

static LONG CALLBACK crashHandler(EXCEPTION_POINTERS *exInfo) {
  if (!exInfo || !exInfo->ExceptionRecord)
    return EXCEPTION_CONTINUE_SEARCH;

  DWORD code = exInfo->ExceptionRecord->ExceptionCode;

  if (!isFatalException(code))
    return EXCEPTION_CONTINUE_SEARCH;

  // Re-entrancy guard
  LONG expected = 0;
  if (!g_CrashHandlerActive.compare_exchange_strong(expected, 1))
    return EXCEPTION_CONTINUE_SEARCH;

  SYSTEMTIME st;
  GetLocalTime(&st);

  int offset = 0;
  int bufSize = sizeof(g_CrashBuffer);

  offset = crashAppend(g_CrashBuffer, bufSize, offset,
                       "=== CRASHLOGGER++ CRASH REPORT ===\n");
  offset = crashAppend(g_CrashBuffer, bufSize, offset,
                       "Timestamp: %04d-%02d-%02d %02d:%02d:%02d.%03d\n",
                       st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                       st.wSecond, st.wMilliseconds);
  offset =
      crashAppend(g_CrashBuffer, bufSize, offset, "Exception: %s (0x%08lX)\n",
                  exceptionCodeToString(code), code);

  // Access violation / in-page error details
  if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) {
    if (exInfo->ExceptionRecord->NumberParameters >= 2) {
      const char *accessType = "UNKNOWN";
      switch (exInfo->ExceptionRecord->ExceptionInformation[0]) {
      case 0:
        accessType = "READ";
        break;
      case 1:
        accessType = "WRITE";
        break;
      case 8:
        accessType = "DEP";
        break;
      }
      offset =
          crashAppend(g_CrashBuffer, bufSize, offset,
                      "  Access type: %s of address 0x%p\n", accessType,
                      reinterpret_cast<void *>(
                          exInfo->ExceptionRecord->ExceptionInformation[1]));
    }
  }

  // Fault address with module+offset
  {
    HMODULE hModule = nullptr;
    char faultModule[256] = "???";
    DWORD64 faultAddr =
        reinterpret_cast<DWORD64>(exInfo->ExceptionRecord->ExceptionAddress);

    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(
                               exInfo->ExceptionRecord->ExceptionAddress),
                           &hModule)) {
      char modulePathA[MAX_PATH];
      if (GetModuleFileNameA(hModule, modulePathA, MAX_PATH)) {
        const char *lastSlash = modulePathA;
        for (const char *p = modulePathA; *p; p++) {
          if (*p == '\\' || *p == '/')
            lastSlash = p + 1;
        }
        int j = 0;
        for (const char *p = lastSlash; *p && j < 255; p++, j++)
          faultModule[j] = *p;
        faultModule[j] = '\0';
      }
      DWORD64 moduleBase = reinterpret_cast<DWORD64>(hModule);
      offset = crashAppend(g_CrashBuffer, bufSize, offset,
                           "  Fault address: %s+0x%llX\n", faultModule,
                           faultAddr - moduleBase);
    } else {
      offset =
          crashAppend(g_CrashBuffer, bufSize, offset, "  Fault address: 0x%p\n",
                      exInfo->ExceptionRecord->ExceptionAddress);
    }
  }

  offset = crashAppend(g_CrashBuffer, bufSize, offset, "Thread: 0x%lX\n",
                       GetCurrentThreadId());

  // Registers first (before RtlVirtualUnwind modifies the context copy)
  if (exInfo->ContextRecord) {
    offset =
        formatRegisters(g_CrashBuffer, bufSize, offset, exInfo->ContextRecord);
    offset =
        formatStackTrace(g_CrashBuffer, bufSize, offset, exInfo->ContextRecord);
  }

  // Module list
  offset = formatModuleList(g_CrashBuffer, bufSize, offset);

  offset = crashAppend(g_CrashBuffer, bufSize, offset,
                       "==================================\n");

  // Write crash report to file using raw Win32 I/O
  {
    wchar_t filePath[MAX_PATH];
    _snwprintf(filePath, MAX_PATH,
               L"%s\\crash_%04d%02d%02d_%02d%02d%02d_%03d_%lu.log",
               g_CrashDirPath, st.wYear, st.wMonth, st.wDay, st.wHour,
               st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentProcessId());

    HANDLE hFile = CreateFileW(filePath, GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
      DWORD bytesWritten = 0;
      WriteFile(hFile, g_CrashBuffer, static_cast<DWORD>(offset), &bytesWritten,
                nullptr);
      FlushFileBuffers(hFile);
      CloseHandle(hFile);
    }
  }

  // Write minidump if enabled
  writeMinidump(exInfo);

  // Let the OS handle termination
  return EXCEPTION_CONTINUE_SEARCH;
}

namespace crashloggerpp {

void install(const wchar_t *crashDirectory, bool enableMinidump) {
  // Guard against double-registration
  if (g_VehHandle)
    return;

  g_MinidumpEnabled = enableMinidump;

  // Set up crash directory path
  if (crashDirectory) {
    wcsncpy(g_CrashDirPath, crashDirectory, MAX_PATH - 1);
    g_CrashDirPath[MAX_PATH - 1] = L'\0';
  } else {
    DWORD len = GetCurrentDirectoryW(MAX_PATH, g_CrashDirPath);
    if (len > 0 && len + 14 < MAX_PATH) {
      _snwprintf(g_CrashDirPath + len, MAX_PATH - len, L"\\logs\\crashes");
    }
  }

  // Create the crash directory (recursive)
  // Walk the path and create each component — CreateDirectoryW silently
  // succeeds if the directory already exists.
  {
    wchar_t tmp[MAX_PATH];
    wcsncpy(tmp, g_CrashDirPath, MAX_PATH - 1);
    tmp[MAX_PATH - 1] = L'\0';
    for (wchar_t *p = tmp; *p; p++) {
      if (*p == L'\\' || *p == L'/') {
        wchar_t saved = *p;
        *p = L'\0';
        CreateDirectoryW(tmp, nullptr);
        *p = saved;
      }
    }
    CreateDirectoryW(tmp, nullptr);
  }

  // Reserve stack space for stack overflow handling
  ULONG stackGuarantee = 32768; // 32KB
  SetThreadStackGuarantee(&stackGuarantee);

#if defined(_M_X64) || defined(__x86_64__)
  // Load NT unwind functions from kernel32 (x64 only)
  HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
  if (hKernel32) {
    g_RtlLookupFunctionEntry = reinterpret_cast<RtlLookupFunctionEntry_t>(
        GetProcAddress(hKernel32, "RtlLookupFunctionEntry"));
    g_RtlVirtualUnwind = reinterpret_cast<RtlVirtualUnwind_t>(
        GetProcAddress(hKernel32, "RtlVirtualUnwind"));
  }
#endif

  // Load DbgHelp dynamically (for symbol resolution and minidumps only)
  g_DbgHelp = LoadLibraryA("dbghelp.dll");
  if (g_DbgHelp) {
    g_SymInitialize = reinterpret_cast<SymInitialize_t>(
        GetProcAddress(g_DbgHelp, "SymInitialize"));
    g_SymFromAddr = reinterpret_cast<SymFromAddr_t>(
        GetProcAddress(g_DbgHelp, "SymFromAddr"));
    g_SymGetLineFromAddr64 = reinterpret_cast<SymGetLineFromAddr64_t>(
        GetProcAddress(g_DbgHelp, "SymGetLineFromAddr64"));
    g_SymCleanup =
        reinterpret_cast<SymCleanup_t>(GetProcAddress(g_DbgHelp, "SymCleanup"));
    g_MiniDumpWriteDump = reinterpret_cast<MiniDumpWriteDump_t>(
        GetProcAddress(g_DbgHelp, "MiniDumpWriteDump"));

    if (g_SymInitialize) {
      g_SymInitialized = g_SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    }
  }

  // Register vectored exception handler (first handler)
  g_VehHandle = AddVectoredExceptionHandler(1, crashHandler);
}

void uninstall() {
  if (g_VehHandle) {
    RemoveVectoredExceptionHandler(g_VehHandle);
    g_VehHandle = nullptr;
  }

  if (g_SymCleanup && g_SymInitialized) {
    g_SymCleanup(GetCurrentProcess());
    g_SymInitialized = false;
  }

  if (g_DbgHelp) {
    FreeLibrary(g_DbgHelp);
    g_DbgHelp = nullptr;
  }

  g_SymInitialize = nullptr;
  g_SymFromAddr = nullptr;
  g_SymGetLineFromAddr64 = nullptr;
  g_SymCleanup = nullptr;
  g_MiniDumpWriteDump = nullptr;

#if defined(_M_X64) || defined(__x86_64__)
  g_RtlLookupFunctionEntry = nullptr;
  g_RtlVirtualUnwind = nullptr;
#endif

  g_MinidumpEnabled = false;
  g_CrashHandlerActive.store(0);
  g_CrashDirPath[0] = L'\0';
}

} // namespace crashloggerpp

extern "C" {

void clpp_install(const wchar_t *crashDirectory, int enableMinidump) {
  crashloggerpp::install(crashDirectory, enableMinidump != 0);
}

void clpp_uninstall() { crashloggerpp::uninstall(); }
}
