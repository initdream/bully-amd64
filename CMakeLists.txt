cmake_minimum_required(VERSION 3.10)
project(bully C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 14)

add_executable(bully
    src/main.c
    src/so_util.c
    src/imports.c
    src/pthread_bridge.c
    src/asset_archive.c
    src/egl_shim.c
    src/jni_shim.c
    src/shadows.c
    src/debug_hooks.c
    src/action_tree_helper.cpp
    src/zip_fs.c
    src/util.c
    src/error.c
)

target_include_directories(bully PRIVATE src)

target_link_libraries(bully PRIVATE
    SDL2
    EGL
    GLESv2
    openal
    mpg123
    m
    pthread
    dl
    stdc++
)
