#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>
#include <openssl/ssl.h>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <string>

namespace net = boost::asio;
namespace ssl = net::ssl;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = net::ip::tcp;
using json = nlohmann::json;

double level_number(const json& level, std::size_t index) {
    return std::stod(level.at(index).get<std::string>());
}

int main() {
    try {
        const std::string host = "data-stream.binance.vision";
        const std::string port = "443";
        const std::string target = "/ws/btcusdt@depth5@100ms";

        net::io_context ioc;
        ssl::context tls_context(ssl::context::tlsv12_client);
        tls_context.set_default_verify_paths();
        tls_context.set_verify_mode(ssl::verify_peer);

        tcp::resolver resolver(ioc);
        websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws(ioc, tls_context);

        auto endpoints = resolver.resolve(host, port);
        beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(30));
        beast::get_lowest_layer(ws).connect(endpoints);

        if (SSL_set_tlsext_host_name(ws.next_layer().native_handle(), host.c_str()) != 1) {
            throw std::runtime_error("Failed to set TLS SNI");
        }
        ws.next_layer().set_verify_callback(ssl::host_name_verification(host));
        ws.next_layer().handshake(ssl::stream_base::client);
        ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
        ws.handshake(host, target);
        beast::get_lowest_layer(ws).expires_never();

        std::cout << "Connected to Binance. Starting PAPER TRADER...\n\n";

        // --- PAPER BROKER STATE ---
        double virtual_cash = 100000.0; // Start with $100,000 USD
        double virtual_inventory = 0.0; // Start with 0 Bitcoin
        double fee_per_share = 0.001;   // 0.1% simulated exchange fee
        
        beast::flat_buffer buffer;

        while (true) {
            ws.read(buffer);
            const std::string payload = beast::buffers_to_string(buffer.data());
            buffer.consume(buffer.size());

            try {
                const json message = json::parse(payload);
                if (!message.contains("bids") || !message.contains("asks")) continue;

                const auto& bids = message.at("bids");
                const auto& asks = message.at("asks");
                if (bids.empty() || asks.empty()) continue;

                double best_bid = level_number(bids.at(0), 0);
                double best_ask = level_number(asks.at(0), 0);
                double bid_qty = level_number(bids.at(0), 1);
                double ask_qty = level_number(asks.at(0), 1);

                double mid_price = (best_bid + best_ask) / 2.0;
                double imbalance = (bid_qty - ask_qty) / (bid_qty + ask_qty);

                // --- STRATEGY: Generate Virtual Quotes ---
                // We want to buy slightly below mid and sell slightly above mid
                double spread = 0.05; // 5 cent spread
                double virtual_bid = mid_price - spread;
                double virtual_ask = mid_price + spread;

                // Inventory Skew: If we have too much Bitcoin, lower our prices to sell it. If we have too little, raise prices to buy it.
                if (virtual_inventory > 0.5) {
                    virtual_bid -= 2.0; 
                    virtual_ask -= 2.0;
                } else if (virtual_inventory < -0.5) {
                    virtual_bid += 2.0; 
                    virtual_ask += 2.0;
                }

                // --- PAPER BROKER: Simulate Fills ---
                // If the live market drops to our virtual bid, we "buy" 0.01 BTC
                if (best_ask <= virtual_bid) {
                    double fill_qty = 0.01;
                    double cost = virtual_bid * fill_qty;
                    double fee = cost * fee_per_share;
                    virtual_cash -= (cost + fee);
                    virtual_inventory += fill_qty;
                    std::cout << "[FILL] BOUGHT 0.01 BTC at $" << virtual_bid << " | Fee: $" << fee << "\n";
                }

                // If the live market rises to our virtual ask, we "sell" 0.01 BTC
                if (best_bid >= virtual_ask && virtual_inventory >= 0.01) {
                    double fill_qty = 0.01;
                    double revenue = virtual_ask * fill_qty;
                    double fee = revenue * fee_per_share;
                    virtual_cash += (revenue - fee);
                    virtual_inventory -= fill_qty;
                    std::cout << "[FILL] SOLD 0.01 BTC at $" << virtual_ask << " | Fee: $" << fee << "\n";
                }

                // --- CALCULATE P&L ---
                double unrealized_pnl = virtual_inventory * mid_price;
                double total_equity = virtual_cash + unrealized_pnl;

                // Print a live dashboard every update
                std::cout << std::fixed << std::setprecision(2);
                std::cout << "BTC: $" << mid_price 
                          << " | Imbalance: " << std::setprecision(2) << imbalance
                          << " | Cash: $" << virtual_cash
                          << " | Inv: " << std::setprecision(4) << virtual_inventory
                          << " BTC | Equity: $" << std::setprecision(2) << total_equity << "\r" << std::flush;

            } catch (const std::exception& e) {
                // Ignore malformed JSON frames
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "\nError: " << e.what() << '\n';
        return 1;
    }
    return 0;
}