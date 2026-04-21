cmake_minimum_required(VERSION 3.22.1)
project("passive_observer" VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Compilation flags ──────────────────────────────────────────────────────────
add_compile_options(
    -Wall
    -Wextra
    -Werror
    -fvisibility=hidden
    -fstack-protector-strong
    -O2
    -fno-exceptions
    -fno-rtti
)

# ── ByteHook vendored paths ────────────────────────────────────────────────────
# Expected on-disk layout after vendoring:
#   libs/bytehook/include/bytehook.h
#   libs/bytehook/${ANDROID_ABI}/libbytehook.so
set(BYTEHOOK_INCLUDE_DIR  ${CMAKE_SOURCE_DIR}/libs/bytehook/include)
set(BYTEHOOK_LIB_DIR      ${CMAKE_SOURCE_DIR}/libs/bytehook/${ANDROID_ABI})
set(BYTEHOOK_SO           ${BYTEHOOK_LIB_DIR}/libbytehook.so)

# Fail fast with a human-readable error if the vendor tree is incomplete.
# This converts a cryptic compiler error into an explicit CMake-time message.
if(NOT EXISTS ${BYTEHOOK_INCLUDE_DIR}/bytehook.h)
    message(FATAL_ERROR
        "[PassiveObserver] bytehook.h not found at ${BYTEHOOK_INCLUDE_DIR}/bytehook.h\n"
        "Run the vendor script:  scripts/vendor_bytehook.sh\n"
        "or download ByteHook from https://github.com/bytedance/bhook/releases\n"
        "and place headers + .so under libs/bytehook/")
endif()

if(NOT EXISTS ${BYTEHOOK_SO})
    message(FATAL_ERROR
        "[PassiveObserver] libbytehook.so not found at ${BYTEHOOK_SO}\n"
        "Ensure you have the prebuilt for ABI=${ANDROID_ABI}")
endif()

# ── Import ByteHook as a prebuilt shared library ───────────────────────────────
add_library(bytehook SHARED IMPORTED)
set_target_properties(bytehook PROPERTIES
    IMPORTED_LOCATION           ${BYTEHOOK_SO}
    INTERFACE_INCLUDE_DIRECTORIES ${BYTEHOOK_INCLUDE_DIR}
)

# ── Main observer shared library ───────────────────────────────────────────────
add_library(observer SHARED
    src/ring_buffer.cpp
    src/dispatcher.cpp
    src/hooks_network.cpp
    src/hooks_crypto.cpp
    src/main.cpp
)

target_include_directories(observer PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${BYTEHOOK_INCLUDE_DIR}
)

target_link_libraries(observer
    bytehook
    log
    android
    dl
)

# ── Copy libbytehook.so alongside libobserver.so so the linker finds it ───────
add_custom_command(TARGET observer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${BYTEHOOK_SO}
        $<TARGET_FILE_DIR:observer>/libbytehook.so
    COMMENT "Staging libbytehook.so next to libobserver.so"
)

# ── Strip debug symbols in Release ────────────────────────────────────────────
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_custom_command(TARGET observer POST_BUILD
        COMMAND ${CMAKE_STRIP} --strip-all $<TARGET_FILE:observer>
        COMMENT "Stripping libobserver.so"
    )
endif()