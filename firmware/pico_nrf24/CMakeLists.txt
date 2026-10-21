# Raspberry Pi Pico / Pico W + nRF24L01+ firmware for ksim live sessions (docs/hardware.md).
# A Pico SDK project of its own, separate from the host build:
#
#   cmake -S firmware/pico_nrf24 -B build-pico                        # Pico
#   cmake -S firmware/pico_nrf24 -B build-pico-w -DPICO_BOARD=pico_w  # Pico W
#   cmake --build build-pico     # -> build-pico/kervan_nrf24.uf2
#
# Uses PICO_SDK_PATH (environment or -D) if set, else downloads the SDK.
cmake_minimum_required(VERSION 3.13...3.31)

set(KERVAN_PICO_SDK_VERSION 2.2.0)

if(NOT PICO_SDK_PATH AND DEFINED ENV{PICO_SDK_PATH})
    set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
endif()
if(NOT PICO_SDK_PATH)
    include(FetchContent)
    FetchContent_Declare(pico_sdk
        GIT_REPOSITORY https://github.com/raspberrypi/pico-sdk.git
        GIT_TAG        ${KERVAN_PICO_SDK_VERSION}
        GIT_SHALLOW    TRUE
        GIT_SUBMODULES lib/tinyusb lib/cyw43-driver lib/lwip
        SOURCE_SUBDIR  none
    )
    FetchContent_MakeAvailable(pico_sdk)
    set(PICO_SDK_PATH ${pico_sdk_SOURCE_DIR})
endif()

if(NOT PICO_BOARD)
    set(PICO_BOARD pico)
endif()
include(${PICO_SDK_PATH}/pico_sdk_init.cmake)

project(kervan_pico_nrf24 C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

set(KFW_COMMON ${CMAKE_CURRENT_LIST_DIR}/../common)
set(KFW_SOURCES
    main.c
    ${KFW_COMMON}/src/app.c
    ${KFW_COMMON}/src/json.c
    ${KFW_COMMON}/src/nrf24.c
)
add_executable(kervan_nrf24 ${KFW_SOURCES})
target_include_directories(kervan_nrf24 PRIVATE ${KFW_COMMON}/include)
target_link_libraries(kervan_nrf24 PRIVATE pico_stdlib pico_unique_id hardware_spi)
if(PICO_CYW43_SUPPORTED)
    target_link_libraries(kervan_nrf24 PRIVATE pico_cyw43_arch_none) # the LED is on the Wi-Fi chip
    target_compile_definitions(kervan_nrf24 PRIVATE KFW_PICO_W=1)
endif()
# Our own sources get the project's warnings; the SDK's do not.
set_source_files_properties(${KFW_SOURCES} PROPERTIES COMPILE_OPTIONS
    "-Wall;-Wextra;-Wshadow;-Werror")

pico_enable_stdio_usb(kervan_nrf24 1)
pico_enable_stdio_uart(kervan_nrf24 0)
pico_set_program_name(kervan_nrf24 "kervan-nrf24")
pico_add_extra_outputs(kervan_nrf24) # .uf2, .bin, .hex
