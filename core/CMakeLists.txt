add_library(kervan_core
    src/kervan.c
)
add_library(kervan::core ALIAS kervan_core)

target_include_directories(kervan_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_compile_definitions(kervan_core PRIVATE
    KV_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    KV_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    KV_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)
target_link_libraries(kervan_core PRIVATE kervan_warnings)
