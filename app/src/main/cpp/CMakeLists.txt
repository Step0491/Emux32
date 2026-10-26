cmake_minimum_required(VERSION 3.22.1)

project("emux_engine")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Configura Boost Header-Only locale incorporato (nessun download esterno runtime)
set(BOOST_MINI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/externals/dynarmic/externals/boost_mini")

if(NOT TARGET Boost::boost)
    add_library(Boost::boost INTERFACE IMPORTED)
    set_target_properties(Boost::boost PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${BOOST_MINI_DIR}"
    )
endif()

set(Boost_FOUND TRUE CACHE BOOL "Boost disponibile localmente" FORCE)

# Opzioni per Dynarmic JIT
set(DYNARMIC_FRONTENDS "A32" CACHE STRING "Abilita solo frontend ARM32" FORCE)
set(DYNARMIC_TESTS OFF CACHE BOOL "Disabilita test" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "Disabilita warning as error" FORCE)
set(DYNARMIC_ENABLE_CPU_FEATURE_DETECTION ON CACHE BOOL "Rileva estensioni ARM64" FORCE)
set(DYNARMIC_USE_PRECOMPILED_HEADERS OFF CACHE BOOL "Disabilita PCH per compatibilità NDK" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "Usa externals inclusi (mcl, fmt, oaknut, robin-map)" FORCE)

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
    ${BOOST_MINI_DIR}
)

find_library(log-lib log)
find_library(android-lib android)
find_library(egl-lib EGL)
find_library(gles-lib GLESv2)

target_link_libraries(
    emux_engine
    dynarmic
    ${log-lib}
    ${android-lib}
    ${egl-lib}
    ${gles-lib}
)
