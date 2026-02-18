add_executable(bench_order_book bench_main.cpp)
target_link_libraries(bench_order_book PRIVATE tape_book)
target_compile_features(bench_order_book PRIVATE cxx_std_20)

# Override -fno-exceptions inherited from INTERFACE target
# (std::map/std::vector in reference implementations need exceptions)
# Use -O3 for meaningful benchmark numbers, -march=native for best codegen
target_compile_options(bench_order_book PRIVATE
  -fexceptions -O3 -march=native
)
