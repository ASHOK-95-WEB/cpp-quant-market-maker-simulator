cmake_minimum_required(VERSION 3.20)
project(quant_lob LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Boost REQUIRED COMPONENTS system)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(CURL REQUIRED)

function(quant_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endfunction()

# Core order-book library
add_library(lob src/order_book.cpp)
target_include_directories(lob PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
quant_enable_warnings(lob)

add_executable(lob_demo src/main.cpp)
target_link_libraries(lob_demo PRIVATE lob)
quant_enable_warnings(lob_demo)

# Unit tests
enable_testing()

add_executable(test_order_book tests/test_order_book.cpp)
target_link_libraries(test_order_book PRIVATE lob)
quant_enable_warnings(test_order_book)
add_test(NAME order_book COMMAND test_order_book)

add_executable(test_local_order_book tests/test_local_order_book.cpp)
quant_enable_warnings(test_local_order_book)
add_test(NAME local_order_book COMMAND test_local_order_book)

add_executable(test_pnl_risk_engine tests/test_pnl_risk_engine.cpp)
quant_enable_warnings(test_pnl_risk_engine)
add_test(NAME pnl_risk_engine COMMAND test_pnl_risk_engine)

# Isolated unit-tested approximation for passive fills inferred from trade prints.
add_executable(test_trade_print_fill_model tests/test_trade_print_fill_model.cpp)
target_include_directories(test_trade_print_fill_model PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/live/paper"
)
quant_enable_warnings(test_trade_print_fill_model)
add_test(NAME trade_print_fill_model COMMAND test_trade_print_fill_model)

# Shared libraries for public Binance market-data applications
set(LIVE_COMMON_LIBS
    Boost::system
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    ws2_32
    crypt32
)

add_executable(binance_live_feed src/live/binance_live_feed.cpp)
target_link_libraries(binance_live_feed PRIVATE ${LIVE_COMMON_LIBS})
quant_enable_warnings(binance_live_feed)

add_executable(binance_local_book src/live/binance_local_book.cpp)
target_link_libraries(binance_local_book PRIVATE
    ${LIVE_COMMON_LIBS}
    CURL::libcurl
)
quant_enable_warnings(binance_local_book)

add_executable(integrated_paper_market_maker
    src/live/integrated_paper_market_maker.cpp
)
target_link_libraries(integrated_paper_market_maker PRIVATE
    ${LIVE_COMMON_LIBS}
    CURL::libcurl
)
quant_enable_warnings(integrated_paper_market_maker)

add_executable(live_paper_trader src/live/live_paper_trader.cpp)
target_link_libraries(live_paper_trader PRIVATE ${LIVE_COMMON_LIBS})
quant_enable_warnings(live_paper_trader)

add_executable(live_stop_loss_bot src/live/live_stop_loss_bot.cpp)
target_link_libraries(live_stop_loss_bot PRIVATE ${LIVE_COMMON_LIBS})
quant_enable_warnings(live_stop_loss_bot)
