include(FetchContent)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

# Treat a third-party target's headers as system headers so our strict warnings skip them.
function(kervan_as_system target)
    get_target_property(dirs ${target} INTERFACE_INCLUDE_DIRECTORIES)
    set_target_properties(${target} PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${dirs}")
endfunction()

FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
)
FetchContent_MakeAvailable(nlohmann_json)
kervan_as_system(nlohmann_json)

# Device profiles (sim/devices/*.json) are compiled into the binary; ksim also loads more at
# runtime with --devices DIR.
file(GLOB KSIM_DEVICE_FILES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/devices/*.json)
list(SORT KSIM_DEVICE_FILES)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${KSIM_DEVICE_FILES})
set(KSIM_DEVICES_CPP "")
foreach(file ${KSIM_DEVICE_FILES})
    file(READ ${file} content)
    string(APPEND KSIM_DEVICES_CPP "    R\"ksim_json(${content})ksim_json\",\n")
endforeach()
configure_file(src/devices_builtin.cpp.in ${CMAKE_CURRENT_BINARY_DIR}/devices_builtin.cpp @ONLY)

# C ABI only: what a stack needs to plug in. No C++ and no Kervan dependency.
add_library(ksim_api INTERFACE)
target_include_directories(ksim_api INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# The simulator. Must not depend on Kervan core.
add_library(ksim_core
    src/channel.cpp
    src/device.cpp
    src/json_util.cpp
    src/lora.cpp
    src/nrf24.cpp
    src/phy.cpp
    src/remote/net.cpp
    src/remote/process.cpp
    src/remote/remote.cpp
    src/scenario.cpp
    src/simulator.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/devices_builtin.cpp
)
target_link_libraries(ksim_core PUBLIC ksim_api PRIVATE nlohmann_json::nlohmann_json kervan_warnings)
target_compile_features(ksim_core PUBLIC cxx_std_20)
target_compile_definitions(ksim_core PRIVATE KSIM_VERSION="${PROJECT_VERSION}")
if(WIN32)
    target_link_libraries(ksim_core PRIVATE ws2_32)
endif()

# Built-in stacks.
add_library(ksim_stack_kervan stacks/kervan/kervan_stack.c)
target_include_directories(ksim_stack_kervan PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/stacks/kervan/include)
target_link_libraries(ksim_stack_kervan PUBLIC ksim_api PRIVATE kervan::core kervan_warnings)

add_library(ksim_stack_nrf24_esb stacks/nrf24_esb/esb_stack.c)
target_include_directories(ksim_stack_nrf24_esb PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/stacks/nrf24_esb/include)
target_link_libraries(ksim_stack_nrf24_esb PUBLIC ksim_api PRIVATE kervan_warnings)

# Out-of-process stacks: C client library and a server for the built-in stacks.
add_library(ksim_remote_client remote/client/ksim_remote.c)
target_include_directories(ksim_remote_client PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/remote/client/include)
target_link_libraries(ksim_remote_client PUBLIC ksim_api PRIVATE kervan_warnings)
if(WIN32)
    target_link_libraries(ksim_remote_client PRIVATE ws2_32)
endif()

add_executable(ksim-node remote/ksim_node.c)
target_link_libraries(ksim-node PRIVATE ksim_remote_client ksim_stack_kervan ksim_stack_nrf24_esb
    kervan_warnings)

add_executable(ksim src/main.cpp)
target_link_libraries(ksim PRIVATE ksim_core ksim_stack_kervan ksim_stack_nrf24_esb kervan_warnings)

if(KERVAN_BUILD_TESTS)
    add_subdirectory(tests)
endif()
