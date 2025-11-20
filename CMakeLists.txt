cmake_minimum_required(VERSION 3.20)
project(tape_book LANGUAGES CXX)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    option(TAPE_BOOK_BUILD_TESTS "Build tape_book tests" ON)
else()
    option(TAPE_BOOK_BUILD_TESTS "Build tape_book tests" OFF)
endif()

add_library(tape_book INTERFACE)
add_library(tape_book::tape_book ALIAS tape_book)

target_include_directories(tape_book INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(tape_book INTERFACE cxx_std_20)

# aggressive-but-sane warnings for downstreams that opt in
if (MSVC)
  target_compile_options(tape_book INTERFACE /W4 /permissive- /EHsc- /GR-)
else()
  target_compile_options(tape_book INTERFACE
    -Wall -Wextra -Wpedantic
    -Wconversion -Wsign-conversion
    -Wshadow -Wdouble-promotion
    -fno-exceptions -fno-rtti
  )
endif()

if (TAPE_BOOK_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
