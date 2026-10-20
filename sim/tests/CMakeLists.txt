FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE) # match the MSVC runtime of our targets
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
kervan_as_system(gtest)
kervan_as_system(gtest_main)

add_executable(ksim_tests
    test_channel.cpp
    test_device.cpp
    test_engine.cpp
    test_esb.cpp
    test_kervan_stack.cpp
    test_lora.cpp
    test_nrf24.cpp
    test_rng.cpp
    test_scenario.cpp
    test_sim.cpp
)
target_link_libraries(ksim_tests PRIVATE ksim_core ksim_stack_kervan ksim_stack_nrf24_esb
    nlohmann_json::nlohmann_json GTest::gtest_main kervan_warnings)
target_compile_definitions(ksim_tests PRIVATE
    KSIM_SCENARIO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../scenarios")

include(GoogleTest)
gtest_discover_tests(ksim_tests DISCOVERY_TIMEOUT 60)
