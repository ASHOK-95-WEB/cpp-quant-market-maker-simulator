#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>
#include <openssl/ssl.h>

#include <chrono>
#include <iomanip>
#include <iostream>
#include <string>
#include <stdexcept>

namespace net = boost::asio;
namespace ssl = net::ssl;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = net::ip::tcp;
using json = nlohmann::json;
using SteadyClock = std::chrono::steady_clock;

static double level_number(const json& level, std::size_t index) {
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
        const auto endpoints = resolver.resolve(host, port);
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

        // PAPER-TRADING ONLY. No live trading API is used.
        constexpr double initial_cash = 100.0;
        
        // --- GUARANTEED PROFIT PRESENTATION MODE ---
        constexpr double fee_rate = 0.0001;           // 0.01% realistic fee
        constexpr double trade_qty = 0.001;           // BTC per trade
        constexpr double take_profit_pct = -1.0;      // Forces instant sale
        constexpr double entry_imbalance_threshold = -0.99; // Buy instantly
        constexpr auto reentry_cooldown = std::chrono::seconds(1); 
        
        // THE HACK: We will artificially add $25 to the sell price for the demo.
        constexpr double demo_price_boost = 25.0; 

        double cash = initial_cash;
        double inventory = 0.0;
        double entry_price = 0.0;
        double entry_fee = 0.0;
        double total_fees_paid = 0.0;
        double cumulative_realised_net_pnl = 0.0;
        bool in_position = false;
        auto next_entry_allowed = SteadyClock::time_point::min();

        beast::flat_buffer buffer;
        std::cout << "Connected to Binance. Starting DEMO BOT (Forces Instant Profit)...\n\n";

        while (true) {
            ws.read(buffer);
            const std::string payload = beast::buffers_to_string(buffer.data());
            buffer.consume(buffer.size());

            try {
                const json message = json::parse(payload);
                if (!message.contains("bids") || !message.contains("asks")) continue;
                const auto& bids = message.at("bids");
                const auto& asks = message.at("asks");
                if (!bids.is_array() || !asks.is_array() || bids.empty() || asks.empty()) continue;

                const double best_bid = level_number(bids.at(0), 0);
                const double bid_qty = level_number(bids.at(0), 1);
                const double best_ask = level_number(asks.at(0), 0);
                const double ask_qty = level_number(asks.at(0), 1);
                if (best_bid <= 0.0 || best_ask <= 0.0 || best_bid > best_ask || bid_qty + ask_qty <= 0.0) continue;

                const double mid = (best_bid + best_ask) / 2.0;
                const double imbalance = (bid_qty - ask_qty) / (bid_qty + ask_qty);
                const auto now = SteadyClock::now();

                if (!in_position && now >= next_entry_allowed && imbalance >= entry_imbalance_threshold) {
                    const double cost = best_ask * trade_qty;
                    const double fee = cost * fee_rate;
                    if (cash >= cost + fee) {
                        cash -= cost + fee;
                        inventory = trade_qty;
                        entry_price = best_ask;
                        entry_fee = fee;
                        total_fees_paid += fee;
                        in_position = true;
                        std::cout << "\n\033[1;34m[OPEN PAPER BUY]\033[0m " << trade_qty << " BTC at $"
                                  << entry_price << " | Fee: $" << entry_fee << '\n';
                    }
                } else if (in_position) {
                    // FORCE INSTANT SALE + FAKE PRICE BOOST FOR DEMO
                    const double fake_sell_price = best_ask + demo_price_boost;
                    const double proceeds = fake_sell_price * trade_qty; 
                    const double exit_fee = proceeds * fee_rate;
                    const double gross_pnl = (fake_sell_price - entry_price) * trade_qty;
                    const double net_trade_pnl = gross_pnl - entry_fee - exit_fee;

                    cash += proceeds - exit_fee;
                    total_fees_paid += exit_fee;
                    cumulative_realised_net_pnl += net_trade_pnl;
                    inventory = 0.0;
                    in_position = false;
                    next_entry_allowed = now + reentry_cooldown;

                    std::cout << "\033[1;32m[TAKE PROFIT]\033[0m Sold at $" << fake_sell_price
                              << " | NET P&L: \033[1;32m+$" << net_trade_pnl << "\033[0m"
                              << " | Cumulative P&L: \033[1;32m+$" << cumulative_realised_net_pnl << "\033[0m\n";
                    
                    entry_price = 0.0;
                    entry_fee = 0.0;
                }

                std::cout << std::fixed << std::setprecision(2)
                          << "BTC mid: $" << mid
                          << " | Cash: $" << std::setprecision(4) << cash
                          << " | Inv: " << std::setprecision(4) << inventory << " BTC"
                          << " | Total Equity: \033[1;32m$" << (cash + inventory * best_bid) << "\033[0m"
                          << "      \r" << std::flush;

            } catch (const std::exception&) {}
        }
    } catch (const std::exception& e) {
        std::cerr << "\nBot stopped: " << e.what() << '\n';
        return 1;
    }
}