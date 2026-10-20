cmake_minimum_required(VERSION 3.21)

project(kervan
    VERSION 0.1.0
    DESCRIPTION "Lightweight, transport-agnostic messaging for embedded wireless networks"
    HOMEPAGE_URL "https://github.com/kervan-mesh/kervan"
    LANGUAGES C)

# Core must stay portable C99: it has to build with MCU toolchains (arm-none-eabi-gcc, xtensa, ...).
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(KERVAN_BUILD_TESTS "Build unit tests" ${PROJECT_IS_TOP_LEVEL})
option(KERVAN_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(KERVAN_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer (GCC/Clang)" OFF)
option(KERVAN_COVERAGE "Build with gcov coverage instrumentation (GCC/Clang)" OFF)
option(KERVAN_BUILD_FUZZERS "Build libFuzzer targets (Clang only, implies KERVAN_SANITIZE)" OFF)

# The UDP port needs an OS with sockets; skip it when cross-compiling for an MCU.
if(PROJECT_IS_TOP_LEVEL AND NOT CMAKE_CROSSCOMPILING)
    set(KERVAN_HOSTED ON)
else()
    set(KERVAN_HOSTED OFF)
endif()
option(KERVAN_BUILD_PORT_UDP "Build the UDP port (PC)" ${KERVAN_HOSTED})
option(KERVAN_BUILD_EXAMPLES "Build examples (needs KERVAN_BUILD_PORT_UDP)" ${KERVAN_HOSTED})
option(KERVAN_BUILD_SIM "Build the ksim simulator (C++20)" ${KERVAN_HOSTED})

if(KERVAN_BUILD_SIM)
    enable_language(CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

# Instrumentation applies to every target (core, tests, Unity) so reports are consistent.
if(KERVAN_BUILD_FUZZERS)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "KERVAN_BUILD_FUZZERS requires Clang")
    endif()
    set(KERVAN_SANITIZE ON)
    add_compile_options(-fsanitize=fuzzer-no-link)
endif()
if(KERVAN_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all
                        -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()
if(KERVAN_COVERAGE)
    add_compile_options(--coverage -O0)
    add_link_options(--coverage)
endif()

add_library(kervan_warnings INTERFACE)
if(MSVC)
    target_compile_options(kervan_warnings INTERFACE /W4 $<$<BOOL:${KERVAN_WARNINGS_AS_ERRORS}>:/WX>)
else()
    target_compile_options(kervan_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
        $<$<BOOL:${KERVAN_WARNINGS_AS_ERRORS}>:-Werror>)
endif()

add_subdirectory(core)

if(KERVAN_BUILD_PORT_UDP)
    add_subdirectory(port/udp)
endif()

if(KERVAN_BUILD_EXAMPLES AND KERVAN_BUILD_PORT_UDP)
    add_subdirectory(examples/udp_demo)
endif()

if(KERVAN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(KERVAN_BUILD_SIM)
    add_subdirectory(sim)
endif()

if(KERVAN_BUILD_FUZZERS)
    add_subdirectory(fuzz)
endif()
