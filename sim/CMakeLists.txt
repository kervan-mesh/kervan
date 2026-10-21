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

# HTTP server for `ksim serve` (header only). SOURCE_SUBDIR points nowhere so its CMake project,
# which would look for OpenSSL, zlib and brotli, is not used.
FetchContent_Declare(httplib
    URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.18.1.tar.gz
    URL_HASH SHA256=405abd8170f2a446fc8612ac635d0db5947c0d2e156e32603403a4496255ff00
    SOURCE_SUBDIR none
)
FetchContent_MakeAvailable(httplib)
add_library(ksim_httplib INTERFACE)
target_include_directories(ksim_httplib SYSTEM INTERFACE ${httplib_SOURCE_DIR})
if(WIN32)
    target_link_libraries(ksim_httplib INTERFACE ws2_32)
endif()

# Hardware catalog (sim/hardware/{mcus,modules,boards}/*.json) is compiled into the binary; ksim
# also loads more at runtime with --hardware DIR.
set(KSIM_HARDWARE_CPP "")
foreach(kind mcu module board)
    file(GLOB files CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/hardware/${kind}s/*.json)
    list(SORT files)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${files})
    foreach(file ${files})
        file(READ ${file} content)
        string(APPEND KSIM_HARDWARE_CPP "    {\"${kind}\", R\"ksim_json(${content})ksim_json\"},\n")
    endforeach()
endforeach()
configure_file(src/hardware_builtin.cpp.in ${CMAKE_CURRENT_BINARY_DIR}/hardware_builtin.cpp @ONLY)

# The helper that runs Python payload functions, embedded so ksim needs no file next to it.
set(KSIM_PAYLOAD_PY_FILE ${CMAKE_CURRENT_SOURCE_DIR}/remote/python/ksim_payload.py)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${KSIM_PAYLOAD_PY_FILE})
file(READ ${KSIM_PAYLOAD_PY_FILE} KSIM_PAYLOAD_PY)
configure_file(src/payload_helper.cpp.in ${CMAKE_CURRENT_BINARY_DIR}/payload_helper.cpp @ONLY)

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
    src/payload_scripts.cpp
    src/phy.cpp
    src/remote/net.cpp
    src/remote/process.cpp
    src/remote/remote.cpp
    src/scenario.cpp
    src/simulator.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/hardware_builtin.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/payload_helper.cpp
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

# `ksim serve`: HTTP API for the web UI (web/). Not part of ksim_core: the simulator needs no network.
add_library(ksim_serve src/serve/api.cpp src/serve/http.cpp)
target_include_directories(ksim_serve PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/serve)
target_link_libraries(ksim_serve PUBLIC ksim_core PRIVATE ksim_httplib nlohmann_json::nlohmann_json
    kervan_warnings)
find_package(Threads REQUIRED)
target_link_libraries(ksim_serve PRIVATE Threads::Threads)

add_executable(ksim src/main.cpp)
target_link_libraries(ksim PRIVATE ksim_core ksim_serve ksim_stack_kervan ksim_stack_nrf24_esb
    kervan_warnings)
# Defaults for `ksim serve` when run from a build tree; --web and --scenarios override them.
target_compile_definitions(ksim PRIVATE
    KSIM_DEFAULT_WEB_DIR="${PROJECT_SOURCE_DIR}/web/dist"
    KSIM_DEFAULT_SCENARIO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/scenarios")

if(KERVAN_BUILD_TESTS)
    add_subdirectory(tests)
endif()
