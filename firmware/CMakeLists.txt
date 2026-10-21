# Board firmware: the portable part (nRF24 driver, protocol JSON, application logic), built on
# every host and cross target. The Pico firmware itself is a separate Pico SDK project
# (firmware/pico_nrf24, docs/hardware.md) that compiles these same sources.
add_library(kervan_fw common/src/app.c common/src/json.c common/src/nrf24.c)
add_library(kervan::fw ALIAS kervan_fw)
target_include_directories(kervan_fw PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/common/include)
target_link_libraries(kervan_fw PRIVATE kervan_warnings)

# Register model of the nRF24L01+ for tests (firmware tests, ksim's virtual boards).
if(KERVAN_HOSTED)
    add_library(kervan_fw_fake testing/fake_nrf24.c)
    target_include_directories(kervan_fw_fake PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/testing)
    target_link_libraries(kervan_fw_fake PUBLIC kervan_fw PRIVATE kervan_warnings)
endif()
