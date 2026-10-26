cmake_minimum_required(VERSION 3.22.1)

project("emux_engine")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Opzioni per Dynarmic JIT
set(DYNARMIC_FRONTENDS "A32" CACHE STRING "Abilita solo frontend ARM32" FORCE)
set(DYNARMIC_TESTS OFF CACHE BOOL "Disabilita test" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "Disabilita warning as error" FORCE)
set(DYNARMIC_ENABLE_CPU_FEATURE_DETECTION ON CACHE BOOL "Rileva estensioni ARM64" FORCE)
set(DYNARMIC_USE_PRECOMPILED_HEADERS OFF CACHE BOOL "Disabilita PCH per compatibilità NDK" FORCE)

# Includi Dynarmic come sottoprogetto CMake
add_subdirectory(externals/dynarmic)

# Compilazione libreria nativa DBT Engine per Android ARM64
add_library(
    emux_engine
    SHARED
    elf_loader.cpp
    linker_hook.cpp
    dbt_engine.cpp
    jni_bridge.cpp
)

target_include_directories(
    emux_engine
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/externals/dynarmic/src
    ${CMAKE_CURRENT_SOURCE_DIR}/externals/dynarmic/externals/mcl/include
    ${CMAKE_CURRENT_SOURCE_DIR}/externals/dynarmic/externals/fmt/include
)

find_library(log-lib log)
find_library(android-lib android)

target_link_libraries(
    emux_engine
    dynarmic
    ${log-lib}
    ${android-lib}
)
