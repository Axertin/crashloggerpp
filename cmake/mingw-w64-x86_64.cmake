# ---------------------------------------------------------------------------
# cmake/mingw-w64-x86_64.cmake  –  Cross-compile toolchain for x86_64 (x64) Windows
# ---------------------------------------------------------------------------
set(CMAKE_SYSTEM_NAME    Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Toolchain prefix – adjust if your distro uses a different prefix
set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)

# Compilers
find_program(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}-gcc)
find_program(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++)
find_program(CMAKE_RC_COMPILER  ${TOOLCHAIN_PREFIX}-windres)

# Target environment search paths
set(CMAKE_FIND_ROOT_PATH /usr/${TOOLCHAIN_PREFIX})

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

# Static runtime by default – avoids shipping libgcc/libstdc++ DLLs
set(CMAKE_EXE_LINKER_FLAGS_INIT    "-static-libgcc -static-libstdc++")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++")
