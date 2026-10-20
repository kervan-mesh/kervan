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

add_library(kervan_warnings INTERFACE)
if(MSVC)
    target_compile_options(kervan_warnings INTERFACE /W4 $<$<BOOL:${KERVAN_WARNINGS_AS_ERRORS}>:/WX>)
else()
    target_compile_options(kervan_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
        $<$<BOOL:${KERVAN_WARNINGS_AS_ERRORS}>:-Werror>)
endif()

add_subdirectory(core)

if(KERVAN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
