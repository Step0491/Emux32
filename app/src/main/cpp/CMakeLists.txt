cmake_minimum_required(VERSION 3.22.1)

project("emux_engine")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Scaricamento automatico FetchContent di Boost Headers (header-only, senza installazioni esterne)
include(FetchContent)

message(STATUS "Download degli header Boost C++ per Dynarmic...")
FetchContent_Declare(
    boost_headers
    URL https://github.com/boostorg/boost/releases/download/boost-1.83.0/boost-1.83.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

# Scarica ed estrae se non presente
FetchContent_GetProperties(boost_headers)
if(NOT boost_headers_POPULATED)
    FetchContent_Populate(boost_headers)
endif()

# Crea il target Boost::boost riconosciuto nativamente da Dynarmic
if(NOT TARGET Boost::boost)
    add_library(Boost::boost INTERFACE IMPORTED)
    set_target_properties(Boost::boost PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${boost_headers_SOURCE_DIR}"
    )
endif()

# Segnala a CMake che Boost è soddisfatto
set(Boost_FOUND TRUE CACHE BOOL "Boost trovato tramite FetchContent" FORCE)

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
    ${boost_headers_SOURCE_DIR}
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
