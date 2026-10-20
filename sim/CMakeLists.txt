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

# C ABI only: what a stack needs to plug in. No C++ and no Kervan dependency.
add_library(ksim_api INTERFACE)
target_include_directories(ksim_api INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# The simulator. Must not depend on Kervan core.
add_library(ksim_core
    src/channel.cpp
    src/lora.cpp
    src/scenario.cpp
    src/simulator.cpp
)
target_link_libraries(ksim_core PUBLIC ksim_api PRIVATE nlohmann_json::nlohmann_json kervan_warnings)
target_compile_features(ksim_core PUBLIC cxx_std_20)

# Built-in stacks.
add_library(ksim_stack_kervan stacks/kervan/kervan_stack.c)
target_include_directories(ksim_stack_kervan PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/stacks/kervan/include)
target_link_libraries(ksim_stack_kervan PUBLIC ksim_api PRIVATE kervan::core kervan_warnings)

add_executable(ksim src/main.cpp)
target_link_libraries(ksim PRIVATE ksim_core ksim_stack_kervan kervan_warnings)

if(KERVAN_BUILD_TESTS)
    add_subdirectory(tests)
endif()
