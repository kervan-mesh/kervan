add_library(kervan_port_udp
    src/udp.c
)
add_library(kervan::port_udp ALIAS kervan_port_udp)

target_include_directories(kervan_port_udp PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(kervan_port_udp PUBLIC kervan::core PRIVATE kervan_warnings)
if(WIN32)
    target_link_libraries(kervan_port_udp PUBLIC ws2_32)
endif()
