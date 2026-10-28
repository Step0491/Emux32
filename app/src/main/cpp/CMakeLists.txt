cmake_minimum_required(VERSION 3.22.1)

project("emux_engine")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Configurazione Dynarmic ARM32 JIT Engine per host Android ARM64
set(ARCHITECTURE "arm64" CACHE STRING "Target host architecture" FORCE)
set(DYNARMIC_FRONTENDS "A32" CACHE STRING "Enable ARM32 frontend" FORCE)
set(DYNARMIC_TESTS OFF CACHE BOOL "Disable tests" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "Disable -Werror" FORCE)
set(DYNARMIC_FATAL_ERRORS OFF CACHE BOOL "Disable -Wfatal-errors" FORCE)
set(DYNARMIC_USE_PRECOMPILED_HEADERS OFF CACHE BOOL "Disable PCH" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "Use bundled libraries" FORCE)
set(DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT OFF CACHE BOOL "Disable W^X" FORCE)
set(DYNARMIC_IGNORE_ASSERTS ON CACHE BOOL "Ignore asserts" FORCE)

add_subdirectory(externals/dynarmic)

# Compilazione libreria nativa Unicorn ARM DBT Engine per Android ARM64
add_library(
    emux_engine
    SHARED
    core/guest_memory.cpp
    core/tb_cache.cpp
    core/syscall_thunker.cpp
    core/audio_thunker.cpp
    core/guest_thread.cpp
    core/gles_virtualizer.cpp
    unicorn/unicorn_impl.cpp
    elf_loader.cpp
    linker_hook.cpp
    dbt_engine.cpp
    jni_bridge.cpp
)

target_compile_definitions(
    emux_engine
    PRIVATE
    EMUX_ENABLE_DYNARMIC=1
)

target_include_directories(
    emux_engine
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/unicorn
    ${CMAKE_CURRENT_SOURCE_DIR}/externals/dynarmic/src
)

find_library(log-lib log)
find_library(android-lib android)
find_library(egl-lib EGL)
find_library(gles-lib GLESv2)
find_library(z-lib z)

target_link_libraries(
    emux_engine
    dynarmic
    ${log-lib}
    ${android-lib}
    ${egl-lib}
    ${gles-lib}
    ${z-lib}
)
