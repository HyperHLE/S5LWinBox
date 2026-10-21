cmake_minimum_required(VERSION 3.22)
project(S5LWinBox C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(S5LBOX_BUILD_HYPERHLE_GLES "Build the optional HyperHLE GLES backend when Cargo is available" ON)

if(MSVC)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

# Portable emulator core & support libraries
add_subdirectory(core)

# SDL2 via FetchContent. Linux display and audio backends need their system development packages.
include(FetchContent)
FetchContent_Declare(
    SDL2
    URL https://github.com/libsdl-org/SDL/releases/download/release-2.28.5/SDL2-2.28.5.zip
)
set(SDL_SHARED TRUE CACHE BOOL "Build shared SDL library" FORCE)
set(SDL_STATIC FALSE CACHE BOOL "Build static SDL library" FORCE)
set(SDL_TEST FALSE CACHE BOOL "Disable SDL tests" FORCE)
FetchContent_MakeAvailable(SDL2)

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
    SDL_MAIN_HANDLED=1
)

if(MSVC)
    target_compile_options(s5lwinbox PRIVATE /W3 /utf-8 /experimental:c11atomics /O2 /Oi /Ot /Oy /GF /Gy)
    target_link_options(s5lwinbox PRIVATE /OPT:REF /OPT:ICF)
else()
    target_compile_options(s5lwinbox PRIVATE -Wall -Wextra -Wno-unused-parameter -O3 -fomit-frame-pointer)
endif()

target_link_libraries(s5lwinbox PRIVATE
    ${S5L_SDL_TARGET}
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

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND S5LBOX_BUILD_HYPERHLE_GLES)
    find_program(CARGO_EXECUTABLE NAMES cargo HINTS "$ENV{HOME}/.cargo/bin")
    if(CARGO_EXECUTABLE)
        set(HYPERHLE_GLES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/rust/hyperhle-gles")
        file(GLOB_RECURSE HYPERHLE_GLES_SOURCES CONFIGURE_DEPENDS
            "${HYPERHLE_GLES_DIR}/src/*.rs"
            "${HYPERHLE_GLES_DIR}/gl-bindings/*.rs"
            "${HYPERHLE_GLES_DIR}/native/*.cpp"
            "${HYPERHLE_GLES_DIR}/third_party/PVRTDecompress/*.cpp"
            "${HYPERHLE_GLES_DIR}/third_party/PVRTDecompress/*.h"
        )
        list(APPEND HYPERHLE_GLES_SOURCES
            "${HYPERHLE_GLES_DIR}/Cargo.toml"
            "${HYPERHLE_GLES_DIR}/build.rs"
            "${HYPERHLE_GLES_DIR}/gl-bindings/Cargo.toml"
            "${HYPERHLE_GLES_DIR}/gl-bindings/build.rs"
        )
        if(EXISTS "${HYPERHLE_GLES_DIR}/Cargo.lock")
            list(APPEND HYPERHLE_GLES_SOURCES "${HYPERHLE_GLES_DIR}/Cargo.lock")
        endif()

        add_custom_target(hyperhle_gles_linux
            COMMAND "${CARGO_EXECUTABLE}" build --release --manifest-path "${HYPERHLE_GLES_DIR}/Cargo.toml"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${HYPERHLE_GLES_DIR}/target/release/libhyperhle_gles.so"
                "$<TARGET_FILE_DIR:s5lwinbox>/libhyperhle_gles.so"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            DEPENDS ${HYPERHLE_GLES_SOURCES}
            COMMENT "Building and staging HyperHLE GLES for Linux"
            VERBATIM
        )
        add_dependencies(s5lwinbox hyperhle_gles_linux)
    else()
        message(STATUS "Cargo not found; S5LWinBox will use its software renderer")
    endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(STATUS "HyperHLE GLES is disabled; S5LWinBox will use its software renderer")
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
                "${CMAKE_SOURCE_DIR}/tools/angle/libGLESv2.dll"
                $<TARGET_FILE_DIR:s5lwinbox>
        )
    endif()
endif()
