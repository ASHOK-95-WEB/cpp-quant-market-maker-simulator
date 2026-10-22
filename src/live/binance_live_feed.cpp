#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>

#include <nlohmann/json.hpp>
#include <openssl/ssl.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

namespace net = boost::asio;
namespace ssl = net::ssl;
namespace beast = boost::beast;
namespace websocket = beast::websocket;

using tcp = net::ip::tcp;
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

// Binance sends price and quantity values as JSON strings.
double level_number(const json& level, std::size_t index) {
    return std::stod(level.at(index).get<std::string>());
}

int main() {
    try {
        const std::string host = "data-stream.binance.vision";
        const std::string port = "443";
        const std::string target =
            "/ws/btcusdt@depth5@100ms";

        // Store logs relative to the project root.
        std::filesystem::create_directories("data");

        const std::string csv_path =
            "data/binance_live_snapshots.csv";

        const bool write_header =
            !std::filesystem::exists(csv_path) ||
            std::filesystem::file_size(csv_path) == 0;

        std::ofstream csv(csv_path, std::ios::app);
        if (!csv) {
            throw std::runtime_error(
                "Cannot open data/binance_live_snapshots.csv");
        }

        if (write_header) {
            csv << "local_receive_ms,last_update_id,"
                   "symbol,best_bid,best_bid_qty,"
                   "best_ask,best_ask_qty,spread,mid_price,"
                   "microprice,bid_depth_5,ask_depth_5,"
                   "imbalance_5,processing_us\n";
            csv.flush();
        }

        net::io_context ioc;
        ssl::context tls_context(ssl::context::tlsv12_client);
        tls_context.set_default_verify_paths();
        tls_context.set_verify_mode(ssl::verify_peer);

        tcp::resolver resolver(ioc);

        websocket::stream<beast::ssl_stream<beast::tcp_stream>>
            ws(ioc, tls_context);

        // Resolve and connect to the secure WebSocket server.
        auto endpoints = resolver.resolve(host, port);

        beast::get_lowest_layer(ws).expires_after(
            std::chrono::seconds(30));

        beast::get_lowest_layer(ws).connect(endpoints);

        // Set TLS SNI and verify the server's certificate/hostname.
        if (SSL_set_tlsext_host_name(
                ws.next_layer().native_handle(),
                host.c_str()) != 1) {
            throw std::runtime_error("Failed to set TLS SNI");
        }

        ws.next_layer().set_verify_callback(
            ssl::host_name_verification(host));

        ws.next_layer().handshake(ssl::stream_base::client);

        ws.set_option(
            websocket::stream_base::timeout::suggested(
                beast::role_type::client));

        ws.handshake(host, target);
        beast::get_lowest_layer(ws).expires_never();

        std::cout << "Connected to Binance public market data.\n";
        std::cout << "Instrument: BTC/USDT\n";
        std::cout << "Stream: depth5 @ 100ms\n";
        std::cout << "Logging to: " << csv_path << "\n";
        std::cout << "Press Ctrl+C to stop.\n\n";

        std::cout << std::fixed << std::setprecision(8);
        std::cout
            << "LocalReceiveMs,UpdateId,Bid,BidQty,Ask,AskQty,"
               "Spread,Mid,Microprice,DepthImbalance,ProcessingUs\n";

        beast::flat_buffer buffer;
        unsigned int rows_since_flush = 0;

        while (true) {
            // Read one complete WebSocket message.
            ws.read(buffer);

            const auto receive_wall = std::chrono::system_clock::now();
            const auto compute_start = Clock::now();

            const std::string payload =
                beast::buffers_to_string(buffer.data());

            buffer.consume(buffer.size());

            try {
                const json message = json::parse(payload);

                // Ignore unexpected/non-depth messages safely.
                if (!message.is_object() ||
                    !message.contains("lastUpdateId") ||
                    !message.contains("bids") ||
                    !message.contains("asks")) {
                    continue;
                }

                const auto& bids = message.at("bids");
                const auto& asks = message.at("asks");

                if (bids.empty() || asks.empty()) {
                    continue;
                }

                const auto update_id =
                    message.at("lastUpdateId").get<std::uint64_t>();

                // The first level is the best bid/ask.
                const double best_bid =
                    level_number(bids.at(0), 0);
                const double best_bid_qty =
                    level_number(bids.at(0), 1);

                const double best_ask =
                    level_number(asks.at(0), 0);
                const double best_ask_qty =
                    level_number(asks.at(0), 1);

                if (best_bid <= 0.0 ||
                    best_ask <= 0.0 ||
                    best_bid > best_ask ||
                    best_bid_qty < 0.0 ||
                    best_ask_qty < 0.0 ||
                    best_bid_qty + best_ask_qty <= 0.0) {
                    continue;
                }

                // Sum the available top-five depth on each side.
                double bid_depth_5 = 0.0;
                double ask_depth_5 = 0.0;

                for (const auto& level : bids) {
                    bid_depth_5 += level_number(level, 1);
                }

                for (const auto& level : asks) {
                    ask_depth_5 += level_number(level, 1);
                }

                const double spread =
                    best_ask - best_bid;

                const double mid_price =
                    (best_bid + best_ask) / 2.0;

                // Top-of-book quantity-weighted microprice.
                const double best_qty =
                    best_bid_qty + best_ask_qty;

                const double microprice =
                    (best_ask * best_bid_qty +
                     best_bid * best_ask_qty) / best_qty;

                const double total_depth =
                    bid_depth_5 + ask_depth_5;

                if (total_depth <= 0.0) {
                    continue;
                }

                const double imbalance_5 =
                    (bid_depth_5 - ask_depth_5) / total_depth;

                const auto processing_end = Clock::now();

                const auto processing_us =
                    std::chrono::duration_cast<
                        std::chrono::microseconds>(
                            processing_end - compute_start).count();

                const auto local_receive_ms =
                    std::chrono::duration_cast<
                        std::chrono::milliseconds>(
                            receive_wall.time_since_epoch()).count();

                // Terminal monitor.
                std::cout
                    << local_receive_ms << ','
                    << update_id << ','
                    << best_bid << ','
                    << best_bid_qty << ','
                    << best_ask << ','
                    << best_ask_qty << ','
                    << spread << ','
                    << mid_price << ','
                    << microprice << ','
                    << std::setprecision(5) << imbalance_5
                    << std::setprecision(8) << ','
                    << processing_us
                    << '\n' << std::flush;

                // CSV history for later backtesting.
                csv
                    << local_receive_ms << ','
                    << update_id << ','
                    << "BTCUSDT" << ','
                    << best_bid << ','
                    << best_bid_qty << ','
                    << best_ask << ','
                    << best_ask_qty << ','
                    << spread << ','
                    << mid_price << ','
                    << microprice << ','
                    << bid_depth_5 << ','
                    << ask_depth_5 << ','
                    << imbalance_5 << ','
                    << processing_us << '\n';

                // Flush periodically, not on every market update.
                if (++rows_since_flush >= 10) {
                    csv.flush();
                    rows_since_flush = 0;
                }

            } catch (const json::exception& e) {
                std::cerr << "Skipping invalid JSON message: "
                          << e.what() << '\n';
            } catch (const std::exception& e) {
                std::cerr << "Skipping invalid market snapshot: "
                          << e.what() << '\n';
            }
        }

    } catch (const std::exception& e) {
        std::cerr << "\nLive feed failed: " << e.what() << '\n';
        return 1;
    }
}