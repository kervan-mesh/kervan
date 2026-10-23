# nRF24L01+ board firmware for ksim live sessions on Zephyr boards (ESP32, STM32, nRF; see
# docs/hardware.md). The radio's pins come from a devicetree overlay that ksim writes for the
# node's wiring (`ksim firmware`), e.g.:
#
#   cmake -S firmware/zephyr_nrf24 -B build-zfw -G Ninja -DBOARD=nucleo_f401re \
#         -DEXTRA_DTC_OVERLAY_FILE=/path/to/wiring.overlay
#   cmake --build build-zfw     # -> build-zfw/zephyr/zephyr.{hex,bin}
#
# Needs ZEPHYR_BASE and the Zephyr SDK (both in the dev container).
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(kervan_zephyr_nrf24 C)

set(KFW_COMMON ${CMAKE_CURRENT_LIST_DIR}/../common)
target_sources(app PRIVATE
    src/main.c
    ${KFW_COMMON}/src/app.c
    ${KFW_COMMON}/src/json.c
    ${KFW_COMMON}/src/nrf24.c
)
target_include_directories(app PRIVATE ${KFW_COMMON}/include)
target_compile_options(app PRIVATE -Wall -Wextra -Wshadow)
