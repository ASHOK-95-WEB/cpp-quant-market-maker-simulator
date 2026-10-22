find_package(Boost REQUIRED COMPONENTS system)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(binance_live_feed
    src/live/binance_live_feed.cpp
)

target_compile_features(binance_live_feed PRIVATE cxx_std_20)

target_link_libraries(binance_live_feed PRIVATE
    Boost::system
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
)