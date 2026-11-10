cmake_minimum_required(VERSION 3.20)

set(PLUGIN_NAME "phonon" CACHE STRING "Plugin name")
project(${PLUGIN_NAME} LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 17)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" PLUGIN_VERSION)
string(STRIP "${PLUGIN_VERSION}" PLUGIN_VERSION)
if(NOT PLUGIN_VERSION MATCHES "^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)$")
    message(FATAL_ERROR "VERSION must be a numeric semantic version")
endif()
set(SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../4D-Plugin-SDK/4D Plugin API")
set(CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../phonon-core")
if(WIN32)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# --- phonon-core: the Phonon engine, a Rust static library -------------------------------
#
# Built with cargo from the pinned Cargo.lock, statically linked into the plugin binary, one
# Rust target per architecture (lipo joins them on macOS).

find_program(CARGO cargo HINTS "$ENV{CARGO_HOME}/bin" "$ENV{HOME}/.cargo/bin" "$ENV{USERPROFILE}/.cargo/bin" REQUIRED)
find_program(RUSTUP rustup HINTS "$ENV{CARGO_HOME}/bin" "$ENV{HOME}/.cargo/bin" "$ENV{USERPROFILE}/.cargo/bin")
set(CARGO_TARGET_DIR "${CMAKE_BINARY_DIR}/cargo")

if(APPLE)
    set(_archs "${CMAKE_OSX_ARCHITECTURES}")
    if(NOT _archs)
        set(_archs "${CMAKE_HOST_SYSTEM_PROCESSOR}")
    endif()
    set(RUST_TARGETS "")
    foreach(_arch IN LISTS _archs)
        if(_arch STREQUAL "arm64" OR _arch STREQUAL "aarch64")
            list(APPEND RUST_TARGETS aarch64-apple-darwin)
        elseif(_arch STREQUAL "x86_64")
            list(APPEND RUST_TARGETS x86_64-apple-darwin)
        else()
            message(FATAL_ERROR "Unsupported macOS architecture: ${_arch}")
        endif()
    endforeach()
    set(_deployment "${CMAKE_OSX_DEPLOYMENT_TARGET}")
    if(NOT _deployment)
        set(_deployment "11.0")
    endif()
    set(CARGO_ENV "MACOSX_DEPLOYMENT_TARGET=${_deployment}")
elseif(WIN32)
    set(RUST_TARGETS x86_64-pc-windows-msvc)
    # Static CRT, like the plugin; AVX2-class baseline, as upstream's release builds use.
    set(CARGO_ENV "RUSTFLAGS=-C target-feature=+crt-static -C target-cpu=x86-64-v3")
else()
    message(FATAL_ERROR "Only macOS and Windows plugins are supported")
endif()

if(RUSTUP)
    foreach(_t IN LISTS RUST_TARGETS)
        execute_process(COMMAND "${RUSTUP}" target add ${_t} RESULT_VARIABLE _r OUTPUT_QUIET ERROR_QUIET)
    endforeach()
endif()

set(CORE_LIBS "")
set(CORE_COMMANDS "")
foreach(_t IN LISTS RUST_TARGETS)
    if(WIN32)
        set(_lib "${CARGO_TARGET_DIR}/${_t}/release/phonon_core.lib")
    else()
        set(_lib "${CARGO_TARGET_DIR}/${_t}/release/libphonon_core.a")
    endif()
    list(APPEND CORE_LIBS "${_lib}")
    if(_t STREQUAL "aarch64-apple-darwin")
        set(_flags "RUSTFLAGS=-C target-cpu=apple-m1")
    elseif(_t STREQUAL "x86_64-apple-darwin")
        set(_flags "RUSTFLAGS=")
    else()
        set(_flags "")
    endif()
    list(APPEND CORE_COMMANDS COMMAND ${CMAKE_COMMAND} -E env ${CARGO_ENV} ${_flags}
        "CARGO_TARGET_DIR=${CARGO_TARGET_DIR}"
        "${CARGO}" build --release --locked --target ${_t}
        --manifest-path "${CORE_DIR}/Cargo.toml")
endforeach()

if(APPLE AND NOT RUST_TARGETS STREQUAL "x86_64-apple-darwin" AND NOT RUST_TARGETS STREQUAL "aarch64-apple-darwin")
    set(CORE_LIB "${CARGO_TARGET_DIR}/universal/libphonon_core.a")
    list(APPEND CORE_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CARGO_TARGET_DIR}/universal"
        COMMAND lipo -create ${CORE_LIBS} -output "${CORE_LIB}")
else()
    list(GET CORE_LIBS 0 CORE_LIB)
endif()

file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS "${CORE_DIR}/src/*.rs")
add_custom_command(
    OUTPUT "${CORE_LIB}"
    ${CORE_COMMANDS}
    DEPENDS ${CORE_SOURCES} "${CORE_DIR}/Cargo.toml" "${CORE_DIR}/Cargo.lock"
    WORKING_DIRECTORY "${CORE_DIR}"
    COMMENT "Building phonon-core (Rust) for ${RUST_TARGETS}"
    VERBATIM)
add_custom_target(phonon_core_build DEPENDS "${CORE_LIB}")

# --- plugin --------------------------------------------------------------------------------

set(PLUGIN_SOURCES ${PLUGIN_NAME}-4dplugin.cpp phonon-audio.cpp "${SDK_DIR}/4DPluginAPI.c")
set(PLUGIN_HEADERS ${PLUGIN_NAME}-4dplugin.h phonon-audio.h phonon-core.h
    "${SDK_DIR}/4DPluginAPI.h" "${SDK_DIR}/EntryPoints.h"
    "${SDK_DIR}/PublicTypes.h" "${SDK_DIR}/PrivateTypes.h" "${SDK_DIR}/Flags.h")
set(PLUGIN_RESOURCES manifest.json constants.xlf)

if(APPLE)
    add_library(${PLUGIN_NAME} MODULE ${PLUGIN_SOURCES} ${PLUGIN_HEADERS} ${PLUGIN_RESOURCES})
    set_target_properties(${PLUGIN_NAME} PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION bundle
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.4d.${PLUGIN_NAME}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PLUGIN_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PLUGIN_VERSION}"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${PLUGIN_NAME}-test/Plugins"
        LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_CURRENT_SOURCE_DIR}/${PLUGIN_NAME}-test/Plugins"
        LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/${PLUGIN_NAME}-test/Plugins")
    set_source_files_properties(${PLUGIN_RESOURCES} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    find_library(COREGRAPHICS_FRAMEWORK CoreGraphics REQUIRED)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
    find_library(AUDIOTOOLBOX_FRAMEWORK AudioToolbox REQUIRED)
    target_link_libraries(${PLUGIN_NAME} PRIVATE
        "${COREGRAPHICS_FRAMEWORK}" "${COREFOUNDATION_FRAMEWORK}" "${AUDIOTOOLBOX_FRAMEWORK}"
        "${CORE_LIB}" iconv)
    set_source_files_properties("${SDK_DIR}/4DPluginAPI.c" PROPERTIES
        COMPILE_FLAGS "-Wno-int-conversion -Wno-multichar")
elseif(WIN32)
    add_library(${PLUGIN_NAME} SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
    set(PLUGIN_OUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/${PLUGIN_NAME}-test/Plugins/${PLUGIN_NAME}/Contents/Windows64")
    set(RESOURCE_OUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/${PLUGIN_NAME}-test/Plugins/${PLUGIN_NAME}/Contents/Resources")
    set_target_properties(${PLUGIN_NAME} PROPERTIES
        SUFFIX ".4DX"
        LINK_FLAGS "/DEF:\"${SDK_DIR}/4DPluginAPI.def\""
        RUNTIME_OUTPUT_DIRECTORY "${PLUGIN_OUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${PLUGIN_OUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${PLUGIN_OUT_DIR}")
    # The libraries the Rust standard library and the engine need, plus waveOut.
    target_link_libraries(${PLUGIN_NAME} PRIVATE
        "${CORE_LIB}" winmm kernel32 ntdll userenv ws2_32 dbghelp bcrypt advapi32)
    add_custom_command(TARGET ${PLUGIN_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${RESOURCE_OUT_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json"
            "${CMAKE_CURRENT_SOURCE_DIR}/constants.xlf" "${RESOURCE_OUT_DIR}")
endif()

add_dependencies(${PLUGIN_NAME} phonon_core_build)
target_include_directories(${PLUGIN_NAME} PRIVATE "${SDK_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
set_source_files_properties("${SDK_DIR}/4DPluginAPI.c" PROPERTIES LANGUAGE CXX)
target_compile_definitions(${PLUGIN_NAME} PRIVATE UNICODE _UNICODE
    PHONON_PLUGIN_VERSION="${PLUGIN_VERSION}")
