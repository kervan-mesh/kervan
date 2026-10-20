add_executable(kervan_udp_demo main.c)
target_link_libraries(kervan_udp_demo PRIVATE kervan::port_udp kervan_warnings)
