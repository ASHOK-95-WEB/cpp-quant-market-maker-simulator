cmake_minimum_required(VERSION 3.20)
project(quant_lob CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Core library: the order book
add_library(lob src/order_book.cpp)
target_include_directories(lob PUBLIC src)
target_compile_options(lob PRIVATE -Wall -Wextra -O2)

# Demo program
add_executable(lob_demo src/main.cpp)
target_link_libraries(lob_demo PRIVATE lob)
target_compile_options(lob_demo PRIVATE -Wall -Wextra -O2)

# Unit tests
enable_testing()
add_executable(test_order_book tests/test_order_book.cpp)
target_link_libraries(test_order_book PRIVATE lob)
target_compile_options(test_order_book PRIVATE -Wall -Wextra)
add_test(NAME order_book COMMAND test_order_book)
