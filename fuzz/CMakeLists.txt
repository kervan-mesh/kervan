# libFuzzer targets. Run e.g.:  ./fuzz_frame -max_total_time=60
function(kervan_add_fuzzer name)
    add_executable(${name} ${name}.c)
    target_link_libraries(${name} PRIVATE kervan::core kervan_warnings)
    target_compile_options(${name} PRIVATE -fsanitize=fuzzer)
    target_link_options(${name} PRIVATE -fsanitize=fuzzer)
endfunction()

kervan_add_fuzzer(fuzz_frame)
kervan_add_fuzzer(fuzz_node)
