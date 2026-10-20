include(FetchContent)

# Unity: the de-facto unit test framework for embedded C.
FetchContent_Declare(unity
    GIT_REPOSITORY https://github.com/ThrowTheSwitch/Unity.git
    GIT_TAG        v2.7.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(unity)

function(kervan_add_test name)
    add_executable(${name} ${name}.c)
    target_link_libraries(${name} PRIVATE kervan::core unity kervan_warnings)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

kervan_add_test(test_frame)
kervan_add_test(test_node)
kervan_add_test(test_version)

if(KERVAN_BUILD_PORT_UDP)
    kervan_add_test(test_udp)
    target_link_libraries(test_udp PRIVATE kervan::port_udp)
endif()
