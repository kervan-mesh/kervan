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
    test_remote.cpp
    test_rng.cpp
    test_scenario.cpp
    test_serve.cpp
    test_sim.cpp
)
find_package(Threads REQUIRED)
find_package(Python3 COMPONENTS Interpreter QUIET)

target_link_libraries(ksim_tests PRIVATE ksim_core ksim_serve ksim_httplib ksim_stack_kervan ksim_stack_nrf24_esb
    ksim_remote_client nlohmann_json::nlohmann_json GTest::gtest_main Threads::Threads
    kervan_warnings)
target_compile_definitions(ksim_tests PRIVATE
    KSIM_SCENARIO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../scenarios"
    KSIM_PY_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../remote/python"
    KSIM_NODE_EXE="$<TARGET_FILE:ksim-node>")
if(Python3_Interpreter_FOUND)
    target_compile_definitions(ksim_tests PRIVATE KSIM_PYTHON="${Python3_EXECUTABLE}")
endif()
add_dependencies(ksim_tests ksim-node)

include(GoogleTest)
gtest_discover_tests(ksim_tests DISCOVERY_TIMEOUT 60)
