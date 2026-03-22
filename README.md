# crashloggerpp

[![Build](https://github.com/Axertin/crashloggerpp/actions/workflows/build.yml/badge.svg)](https://github.com/Axertin/crashloggerpp/actions/workflows/build.yml)

A Windows DLL that installs a vectored exception handler (VEH) to catch fatal crashes in the host process and log detailed reports. Intended to be loaded by another module that calls into it to start and stop crash logging.

## What it logs

When a fatal exception occurs (access violation, stack overflow, divide by zero, illegal instruction, etc.), crashloggerpp writes a `.log` file containing:

- Timestamp and exception type
- Access violation details (read/write/DEP and target address)
- Faulting module and offset
- Thread ID
- Stack trace with symbol resolution (walked via `RtlVirtualUnwind` on x64, EBP chain on x86; symbols resolved via DbgHelp when available)
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

Replace `debug` with `release` for optimized builds. Native MSVC presets (`native-x86-release`, `native-x64-release`, etc.) are also available for building on Windows directly. Since these use Ninja, the target architecture is determined by your MSVC environment: run from the matching VS Developer Command Prompt (x86 or x64). In CI, this is handled by `ilammy/msvc-dev-cmd`.

The output DLL is in `build/<preset>/`.

DbgHelp (`dbghelp.dll`) is loaded dynamically at runtime for symbol resolution and minidump support. It is not a build-time dependency — the DLL only links against `psapi`.

## Usage

crashloggerpp is designed to be loaded into a target process at runtime by another module, either injected or chain-loaded. A standalone consumer header is provided for easy integration, or you can resolve the exports manually.

### Using the consumer header

The easiest way to use crashloggerpp is with the provided `crashloggerpp.h` header, which handles loading the DLL and resolving symbols for you. No link-time dependency is needed, just drop the header and DLL into your project.

```cpp
#include "crashloggerpp.h"

// Load the DLL (returns false on failure)
crashloggerpp::load(L"path/to/crashloggerpp.dll");

// Install the crash handler
crashloggerpp::install(L"C:\\MyApp\\crashes", true);

// On shutdown — uninstalls the handler and frees the DLL
crashloggerpp::uninstall();
```

`load()` with no arguments looks for `crashloggerpp.dll` in the standard DLL search path. `uninstall()` removes the handler and frees the DLL in one call.

### Manual loading

If you prefer not to use the consumer header, you can load the DLL and resolve the `extern "C"` exports directly:

```cpp
HMODULE hCrashLogger = LoadLibraryW(L"crashloggerpp.dll");

auto install = reinterpret_cast<void (*)(const wchar_t *, int)>(
    GetProcAddress(hCrashLogger, "clpp_install"));
auto uninstall = reinterpret_cast<void (*)()>(
    GetProcAddress(hCrashLogger, "clpp_uninstall"));

install(L"C:\\MyApp\\crashes", 1);

// On shutdown
uninstall();
FreeLibrary(hCrashLogger);
```

### Parameters

| Parameter        | Type             | Default   | Description                                                                 |
| ---------------- | ---------------- | --------- | --------------------------------------------------------------------------- |
| `crashDirectory` | `const wchar_t*` | `nullptr` | Absolute path for crash output. `nullptr` defaults to `<cwd>\logs\crashes`. |
| `enableMinidump` | `bool`           | `false`   | Write a `.dmp` minidump file alongside the crash log.                       |

Note: the raw `extern "C"` exports (`clpp_install`, `clpp_uninstall`) use `int` instead of `bool` for C ABI compatibility. The consumer header handles this conversion automatically.

Calling `install()` multiple times is safe. Subsequent calls are ignored while a handler is already registered. Call `uninstall()` first if you need to reconfigure.

### Language examples

#### C#

```csharp
using System.Runtime.InteropServices;

static class CrashLoggerPP
{
    [DllImport("crashloggerpp.dll", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    public static extern void clpp_install(string? crashDirectory, int enableMinidump);

    [DllImport("crashloggerpp.dll", CallingConvention = CallingConvention.Cdecl)]
    public static extern void clpp_uninstall();
}

// Usage
CrashLoggerPP.clpp_install(@"C:\MyApp\crashes", 1);

// On shutdown
CrashLoggerPP.clpp_uninstall();
```

#### Rust

```rust
extern "C" {
    fn clpp_install(crash_directory: *const u16, enable_minidump: i32);
    fn clpp_uninstall();
}

// Usage (with a null-terminated UTF-16 path)
let path: Vec<u16> = "C:\\MyApp\\crashes\0".encode_utf16().collect();
unsafe { clpp_install(path.as_ptr(), 1) };

// On shutdown
unsafe { clpp_uninstall() };
```

Or load at runtime with `libloading`:

```rust
let lib = unsafe { libloading::Library::new("crashloggerpp.dll").unwrap() };
let install: libloading::Symbol<unsafe extern "C" fn(*const u16, i32)> =
    unsafe { lib.get(b"clpp_install").unwrap() };
let uninstall: libloading::Symbol<unsafe extern "C" fn()> =
    unsafe { lib.get(b"clpp_uninstall").unwrap() };
```

#### Lua (LuaJIT FFI)

```lua
local ffi = require("ffi")

ffi.cdef[[
    void clpp_install(const wchar_t *crashDirectory, int enableMinidump);
    void clpp_uninstall();
]]

local clpp = ffi.load("crashloggerpp")

-- Install with defaults
clpp.clpp_install(nil, 0)

-- Or with a specific path
local path = ffi.new("wchar_t[?]", 32)
ffi.copy(path, "C:\\MyApp\\crashes", 30)  -- wchar_t copy
clpp.clpp_install(path, 1)

-- On shutdown
clpp.clpp_uninstall()
```

#### Python (ctypes)

```python
import ctypes

clpp = ctypes.CDLL("crashloggerpp.dll")
clpp.clpp_install.argtypes = [ctypes.c_wchar_p, ctypes.c_int]
clpp.clpp_install.restype = None
clpp.clpp_uninstall.argtypes = []
clpp.clpp_uninstall.restype = None

clpp.clpp_install(r"C:\MyApp\crashes", 1)

# On shutdown
clpp.clpp_uninstall()
```

## License

[MIT](LICENSE)
