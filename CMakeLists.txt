cmake_minimum_required(VERSION 3.25)

project(CrashLoggerPP
    VERSION   0.1.0
    LANGUAGES C CXX
)

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD          23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Export compile_commands.json for clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Target: crashloggerpp  (shared library / DLL)
# ---------------------------------------------------------------------------
add_library(crashloggerpp SHARED
    src/dllmain.cpp
    src/crash_handler.cpp
)

target_compile_definitions(crashloggerpp PRIVATE CRASHLOGGERPP_EXPORTS)

target_include_directories(crashloggerpp
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(crashloggerpp
    PRIVATE
        psapi
)

# MinGW-specific: statically link runtime so DLL is self-contained
if(MINGW)
    target_link_options(crashloggerpp PRIVATE
        -static-libgcc
        -static-libstdc++
    )
endif()

# ---------------------------------------------------------------------------
# Symlink compile_commands.json to the repo root for clangd
# ---------------------------------------------------------------------------
if(CMAKE_EXPORT_COMPILE_COMMANDS)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E create_symlink
            "${CMAKE_BINARY_DIR}/compile_commands.json"
            "${CMAKE_SOURCE_DIR}/compile_commands.json"
    )
endif()
