cmake_minimum_required(VERSION 3.22.1)

project("emux_engine")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Compilazione libreria nativa DBT Engine per Android ARM64
add_library(
    emux_engine
    SHARED
    elf_loader.cpp
    linker_hook.cpp
    dbt_engine.cpp
    jni_bridge.cpp
)

find_library(log-lib log)
find_library(android-lib android)

target_link_libraries(
    emux_engine
    ${log-lib}
    ${android-lib}
)
