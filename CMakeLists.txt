cmake_minimum_required(VERSION 3.16)
project(psychonauts_recomp C ASM)

option(PSYCHOSW_USE_GAME_ICON "Embed the local game icon in the Switch NRO" ON)
option(NINTENDO_SWITCH "Build for Nintendo Switch (libnx)" OFF)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

if(SWITCH OR NINTENDO_SWITCH)
    set(IS_SWITCH TRUE)
else()
    set(IS_SWITCH FALSE)
endif()

file(GLOB GEN_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/gen/recomp_*.c")

set(RUNTIME_SOURCES
    src/runtime/main.c
    src/runtime/memory.c
    src/runtime/settings.c
    src/runtime/widescreen.c
    src/runtime/postfx.c
    src/runtime/launcher_ui.c
    src/runtime/dispatch.c
    src/runtime/hle_jmp.c
    src/runtime/hle_libc.c
    src/runtime/hle_sdl12.c
    src/runtime/hle_gl.c
    src/runtime/hle_openal.c
    src/runtime/hle_bink.c
    src/runtime/hle_steam.c
)

if(IS_SWITCH)
    list(APPEND RUNTIME_SOURCES src/runtime/fiber_aarch64.S)
endif()

add_executable(psychonauts_recomp
    ${RUNTIME_SOURCES}
    ${GEN_SOURCES}
)

target_include_directories(psychonauts_recomp PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gen"
)

# Common warnings suppression and optimization flags for lifted C code.
set(RECOMP_GEN_OPT "-O2" CACHE STRING "Compiler optimization flag for locally translated C")
set(RECOMP_GEN_FLAGS
    ${RECOMP_GEN_OPT}
    -fno-strict-aliasing
    -fno-math-errno
    -fno-trapping-math
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-unused-function
    -Wno-unused-label
    -Wno-parentheses
    -Wno-shift-negative-value
)
set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "${RECOMP_GEN_FLAGS}")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/gen/recomp_00.c" PROPERTIES COMPILE_OPTIONS "-O3;-fno-strict-aliasing;-fno-math-errno;-fno-trapping-math;-Wno-unused-variable;-Wno-unused-but-set-variable;-Wno-unused-function;-Wno-unused-label;-Wno-parentheses;-Wno-shift-negative-value")
set_source_files_properties(${RUNTIME_SOURCES} PROPERTIES COMPILE_OPTIONS "-O3;-fno-strict-aliasing;-fno-math-errno;-fno-trapping-math")

target_compile_options(psychonauts_recomp PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-fno-strict-aliasing>
    $<$<COMPILE_LANGUAGE:C>:-Wall>
    $<$<COMPILE_LANGUAGE:C>:-Wextra>
    $<$<COMPILE_LANGUAGE:C>:-Wno-unused-parameter>
    $<$<COMPILE_LANGUAGE:C>:-Wno-unused-function>
    $<$<COMPILE_LANGUAGE:C>:-Wno-sign-compare>
    $<$<COMPILE_LANGUAGE:C>:-Wno-missing-field-initializers>
    $<$<COMPILE_LANGUAGE:C>:-Wno-format-truncation>
)

if(IS_SWITCH)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SWITCH_DEPS REQUIRED
        sdl2
        openal
        libavformat
        libavcodec
        libswscale
        libswresample
        libavutil
    )
    target_compile_definitions(psychonauts_recomp PRIVATE
        __SWITCH__=1
        AL_LIBTYPE_STATIC=1
    )
    target_include_directories(psychonauts_recomp PRIVATE
        ${SWITCH_DEPS_INCLUDE_DIRS}
        "/opt/devkitpro/portlibs/switch/include"
        "/opt/devkitpro/libnx/include"
    )
    target_link_directories(psychonauts_recomp PRIVATE
        ${SWITCH_DEPS_STATIC_LIBRARY_DIRS}
        ${SWITCH_DEPS_LIBRARY_DIRS}
        "/opt/devkitpro/portlibs/switch/lib"
        "/opt/devkitpro/libnx/lib"
    )
    target_link_libraries(psychonauts_recomp PRIVATE
        ${SWITCH_DEPS_STATIC_LIBRARIES}
        EGL
        glapi
        drm_nouveau
        stdc++
        nx
        m
        pthread
    )
    target_link_options(psychonauts_recomp PRIVATE
        -Wl,--wrap=threadCreate
        -Wl,--wrap=SDL_SYS_SetThreadPriority
        -Wl,--wrap=nwindowDequeueBuffer
    )
    set_target_properties(psychonauts_recomp PROPERTIES
        OUTPUT_NAME "psychonauts_recomp"
        SUFFIX ".elf"
    )
    set(NRO_ICON_OPTION)
    if(PSYCHOSW_USE_GAME_ICON AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.jpg")
        set(NRO_ICON_OPTION "--icon=${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.jpg")
    endif()
    add_custom_command(TARGET psychonauts_recomp POST_BUILD
        COMMAND nacptool --create "Psychonauts" "PsychoSW contributors" "1.0.0" "${CMAKE_CURRENT_BINARY_DIR}/Psychonauts.nacp"
        COMMAND elf2nro "$<TARGET_FILE:psychonauts_recomp>" "${CMAKE_CURRENT_BINARY_DIR}/Psychonauts.nro"
                ${NRO_ICON_OPTION}
                "--nacp=${CMAKE_CURRENT_BINARY_DIR}/Psychonauts.nacp"
        COMMENT "Packing Psychonauts.nro"
    )
else()
    find_package(PkgConfig REQUIRED)
    find_package(Threads REQUIRED)
    pkg_check_modules(HOST_DEPS REQUIRED
        sdl2
        openal
        gl
        libavformat
        libavcodec
        libswscale
        libswresample
        libavutil
    )
    target_compile_options(psychonauts_recomp PRIVATE
        $<$<COMPILE_LANGUAGE:C>:-O2>
        $<$<COMPILE_LANGUAGE:C>:-fno-math-errno>
        $<$<COMPILE_LANGUAGE:C>:-fno-trapping-math>
        $<$<COMPILE_LANGUAGE:C>:-g>
    )
    target_include_directories(psychonauts_recomp PRIVATE ${HOST_DEPS_INCLUDE_DIRS})
    target_link_directories(psychonauts_recomp PRIVATE ${HOST_DEPS_LIBRARY_DIRS})
    target_link_libraries(psychonauts_recomp PRIVATE
        ${HOST_DEPS_LIBRARIES}
        Threads::Threads
        m
        dl
    )
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
        target_link_options(psychonauts_recomp PRIVATE -fuse-ld=lld)
    endif()
endif()
