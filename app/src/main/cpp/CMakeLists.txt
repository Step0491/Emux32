cmake_minimum_required(VERSION 3.22.1)

project("emux_engine")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Compilazione libreria nativa Unicorn ARM DBT Engine per Android ARM64
add_library(
    emux_engine
    SHARED
    unicorn/unicorn_impl.cpp
    elf_loader.cpp
    linker_hook.cpp
    dbt_engine.cpp
    jni_bridge.cpp
)

target_include_directories(
    emux_engine
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/unicorn
)

find_library(log-lib log)
find_library(android-lib android)
find_library(egl-lib EGL)
find_library(gles-lib GLESv2)
find_library(z-lib z)

target_link_libraries(
    emux_engine
    ${log-lib}
    ${android-lib}
    ${egl-lib}
    ${gles-lib}
    ${z-lib}
)
