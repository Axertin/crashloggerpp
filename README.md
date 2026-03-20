# crashloggerpp

A Windows DLL that installs a vectored exception handler (VEH) to catch fatal crashes in the host process and log detailed reports. Intended to be loaded by another module that calls into it to start and stop crash logging.

## What it logs

When a fatal exception occurs (access violation, stack overflow, divide by zero, illegal instruction, etc.), crashloggerpp writes a `.log` file containing:

- Timestamp and exception type
- Access violation details (read/write/DEP and target address)
- Faulting module and offset
- Thread ID
- Stack trace with symbol resolution (via DbgHelp)
- Register dump (x86 or x64 depending on build)
- List of all loaded modules

Optionally, a `.dmp` minidump file can be written alongside the log.

After writing the report, the handler returns `EXCEPTION_CONTINUE_SEARCH` so the OS can proceed with normal termination.

## Building

Requires CMake 3.25+ and Ninja. Cross-compilation from Linux uses the MinGW-w64 toolchain.

```bash
# x86 (i686)
cmake --preset mingw-x86-debug
cmake --build --preset mingw-x86-debug

# x64
cmake --preset mingw-x64-debug
cmake --build --preset mingw-x64-debug
```

Replace `debug` with `release` for optimized builds. Native MSVC/Clang presets (`native-debug`, `native-release`) are also available for building on Windows directly.

The output DLL is in `build/<preset>/libcrashloggerpp.dll`.

## Usage

crashloggerpp is designed to be loaded into a target process at runtime by another module — either injected or chain-loaded. The loading module is responsible for calling `install()` and `uninstall()` via `GetProcAddress`.

### Loading and calling from another module

```cpp
// Load the DLL into the process
HMODULE hCrashLogger = LoadLibraryW(L"libcrashloggerpp.dll");

// Resolve the exported functions
using InstallFn = void (*)(const wchar_t *, int);
using UninstallFn = void (*)();

auto install = reinterpret_cast<InstallFn>(GetProcAddress(hCrashLogger, "clpp_install"));
auto uninstall = reinterpret_cast<UninstallFn>(GetProcAddress(hCrashLogger, "clpp_uninstall"));

// Install with defaults (logs to <cwd>\logs\crashes, no minidumps)
install(nullptr, 0);

// Or specify a crash directory and enable minidumps
install(L"C:\\MyApp\\crashes", 1);

// On shutdown
uninstall();
FreeLibrary(hCrashLogger);
```

### Parameters

| Parameter | Type | Default | Description |
|---|---|---|---|
| `crashDirectory` | `const wchar_t*` | `nullptr` | Absolute path for crash output. `nullptr` defaults to `<cwd>\logs\crashes`. |
| `enableMinidump` | `bool` | `false` | Write a `.dmp` minidump file alongside the crash log. |
