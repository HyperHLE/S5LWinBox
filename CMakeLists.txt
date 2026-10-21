cmake_minimum_required(VERSION 3.22)
project(S5LWinBox C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

# Boost detection for Dynarmic on Windows CI runners
if(WIN32)
    if(NOT DEFINED BOOST_ROOT AND NOT DEFINED ENV{BOOST_ROOT})
        file(GLOB BOOST_HOSTED "C:/hostedtoolcache/windows/Boost/*/*")
        if(BOOST_HOSTED)
            list(GET BOOST_HOSTED -1 BOOST_LATEST)
            set(ENV{BOOST_ROOT} "${BOOST_LATEST}")
            set(BOOST_ROOT "${BOOST_LATEST}" CACHE PATH "" FORCE)
            message(STATUS "Auto-detected BOOST_ROOT: ${BOOST_ROOT}")
        endif()
    endif()
endif()

# Dynarmic A32 JIT configuration
set(DYNARMIC_FRONTENDS "A32" CACHE STRING "" FORCE)
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_FATAL_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)

# Portable emulator core & support libraries
add_subdirectory(core)

# SDL2 via FetchContent (clean, self-contained, no external dev packages required)
include(FetchContent)
FetchContent_Declare(
    SDL2
    URL https://github.com/libsdl-org/SDL/releases/download/release-2.28.5/SDL2-2.28.5.zip
)
set(SDL_SHARED TRUE CACHE BOOL "Build shared SDL library" FORCE)
set(SDL_STATIC FALSE CACHE BOOL "Build static SDL library" FORCE)
set(SDL_TEST FALSE CACHE BOOL "Disable SDL tests" FORCE)
FetchContent_MakeAvailable(SDL2)

# Dynarmic A32 via FetchContent
FetchContent_Declare(
    dynarmic
    GIT_REPOSITORY https://github.com/lioncash/dynarmic.git
    GIT_TAG master
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(dynarmic)

# Determine SDL2 target name
if(TARGET SDL2::SDL2)
    set(S5L_SDL_TARGET SDL2::SDL2)
elseif(TARGET SDL2)
    set(S5L_SDL_TARGET SDL2)
elseif(TARGET SDL2-static)
    set(S5L_SDL_TARGET SDL2-static)
endif()

# Collect portable app sources (excluding VMFirmwareHLE.c which is already in host_vm_firmware_hle)
file(GLOB S5L_APP_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/app/Sources/*.c")
list(FILTER S5L_APP_SOURCES EXCLUDE REGEX "/VMFirmwareHLE\\.c$")

add_executable(s5lwinbox
    src/main.c
    src/ui.c
    src/engine.c
    src/engine_ipa.c
    src/web_log_server.c
    src/dynarmic_jit.cpp
    ${S5L_APP_SOURCES}
)

target_include_directories(s5lwinbox PRIVATE
    src
    core/include
    app/Sources
    tools
)

target_compile_definitions(s5lwinbox PRIVATE
    _FILE_OFFSET_BITS=64
    S5LBOX_DESKTOP=1
    S5LBOX_IOS3_HLE_EXPERIMENT=1
    S5LBOX_IOS_WFI_REALTIME_PACING=1
    S5LBOX_IOS_ACTIVE_REALTIME_CLOCK=1
    S5LBOX_DYNARMIC_JIT=1
    SDL_MAIN_HANDLED=1
)

if(MSVC)
    target_compile_options(s5lwinbox PRIVATE /W3 /utf-8 $<$<COMPILE_LANGUAGE:C>:/experimental:c11atomics> /O2 /Oi /Ot /Oy /GF /Gy)
    target_link_options(s5lwinbox PRIVATE /OPT:REF /OPT:ICF)
else()
    target_compile_options(s5lwinbox PRIVATE -Wall -Wextra -Wno-unused-parameter -O3 -fomit-frame-pointer)
endif()

target_link_libraries(s5lwinbox PRIVATE
    ${S5L_SDL_TARGET}
    dynarmic
    emucore
    nethost
    host_file_block
    host_rootfs_work
    host_ios3_bringup_gate
    host_ios3_kernel_patch
    host_ios3_hle
    host_vm_firmware_hle
    host_guest_patch
    host_payload_tar
    host_sha256
)

if(WIN32)
    target_link_libraries(s5lwinbox PRIVATE ws2_32)
else()
    target_link_libraries(s5lwinbox PRIVATE m dl)
endif()

# Copy SDL2.dll and ANGLE DLLs next to executable on Windows
if(WIN32)
    if(TARGET ${S5L_SDL_TARGET})
        add_custom_command(TARGET s5lwinbox POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:${S5L_SDL_TARGET}>
                $<TARGET_FILE_DIR:s5lwinbox>
        )
    endif()
    if(EXISTS "${CMAKE_SOURCE_DIR}/tools/angle/libEGL.dll")
        add_custom_command(TARGET s5lwinbox POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/tools/angle/libEGL.dll"
                $<TARGET_FILE_DIR:s5lwinbox>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/tools/angle/libGLESv2.dll"
                $<TARGET_FILE_DIR:s5lwinbox>
        )
    endif()
endif()
