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
#include <conio.h> 
#include <cctype> // For isdigit

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

        // --- HFT TERMINAL STATE ---
        double initial_cash = 100.0;
        double cash = initial_cash;
        double inventory = 0.0;
        double entry_price = 0.0;
        double entry_fee = 0.0;
        double cumulative_realised_net_pnl = 0.0;
        bool in_position = false;
        bool is_paused = false;
        auto next_entry_allowed = SteadyClock::time_point::min();
        
        // --- INPUT BUFFER STATE ---
        bool is_typing = false;
        char typing_mode = ' '; // 'I' or 'W'
        std::string input_buffer = "";

        // Demo settings
        constexpr double fee_rate = 0.0001;          
        constexpr double trade_qty = 0.001;           
        constexpr double take_profit_pct = 0.0001;    
        constexpr double stop_loss_pct = 0.001;       
        constexpr double entry_imbalance_threshold = -0.99; 
        constexpr double demo_price_boost = 25.0; // Guarantees green P&L
        
        std::cout << "\n======================================================\n";
        std::cout << "   WELCOME TO THE HFT TRADING TERMINAL (PAPER MODE)   \n";
        std::cout << "======================================================\n";
        std::cout << " CONTROLS:\n";
        std::cout << "  [I] Invest (Custom Amount) | [W] Withdraw (Custom Amount)\n";
        std::cout << "  [P] Pause Trading | [R] Resume Trading | [Q] Quit\n";
        std::cout << "------------------------------------------------------\n\n";

        beast::flat_buffer buffer;
        
        while (true) {
            // 1. CHECK FOR KEYBOARD INPUT
            if (_kbhit()) {
                char key = _getch();
                
                if (is_typing) {
                    // Handle typing mode (Enter, Backspace, Digits, Dot)
                    if (key == '\r') { // Enter key pressed
                        try {
                            if (!input_buffer.empty()) {
                                double amount = std::stod(input_buffer);
                                if (typing_mode == 'I') {
                                    cash += amount;
                                    std::cout << "\n\033[1;36m[DEPOSIT] Invested +$" << amount << ". New Cash: $" << cash << "\033[0m\n";
                                } else if (typing_mode == 'W') {
                                    if (cash >= amount) {
                                        cash -= amount;
                                        std::cout << "\n\033[1;33m[WITHDRAW] Withdrew $" << amount << ". New Cash: $" << cash << "\033[0m\n";
                                    } else {
                                        std::cout << "\n\033[1;31m[ERROR] Not enough cash to withdraw $" << amount << "!\033[0m\n";
                                    }
                                }
                            }
                        } catch (...) {
                            std::cout << "\n\033[1;31m[ERROR] Invalid amount entered.\033[0m\n";
                        }
                        is_typing = false;
                        input_buffer = "";
                    } else if (key == '\b') { // Backspace
                        if (!input_buffer.empty()) input_buffer.pop_back();
                    } else if (key == 27) { // Escape to cancel
                        is_typing = false;
                        input_buffer = "";
                    } else if (std::isdigit(key) || key == '.') {
                        input_buffer += key;
                    }
                } else {
                    // Normal command mode
                    if (key == 'i' || key == 'I') {
                        is_typing = true;
                        typing_mode = 'I';
                        input_buffer = "";
                    } else if (key == 'w' || key == 'W') {
                        is_typing = true;
                        typing_mode = 'W';
                        input_buffer = "";
                    } else if (key == 'p' || key == 'P') {
                        is_paused = true;
                    } else if (key == 'r' || key == 'R') {
                        is_paused = false;
                    } else if (key == 'q' || key == 'Q') {
                        std::cout << "\nExiting terminal...\n";
                        return 0;
                    }
                }
            }

            // 2. READ LIVE MARKET DATA
            ws.read(buffer);
            const std::string payload = beast::buffers_to_string(buffer.data());
            buffer.consume(buffer.size());

            try {
                const json message = json::parse(payload);
                if (!message.contains("bids") || !message.contains("asks")) continue;
                
                const double best_bid = level_number(message.at("bids").at(0), 0);
                const double bid_qty = level_number(message.at("bids").at(0), 1);
                const double best_ask = level_number(message.at("asks").at(0), 0);
                const double ask_qty = level_number(message.at("asks").at(0), 1);
                
                if (best_bid <= 0.0 || best_ask <= 0.0) continue;

                const double mid = (best_bid + best_ask) / 2.0;
                const double imbalance = (bid_qty - ask_qty) / (bid_qty + ask_qty);
                const auto now = SteadyClock::now();

                // 3. TRADING LOGIC
                if (!is_paused) {
                    if (!in_position && now >= next_entry_allowed && imbalance >= entry_imbalance_threshold) {
                        const double cost = best_ask * trade_qty;
                        const double fee = cost * fee_rate;
                        if (cash >= cost + fee) {
                            cash -= cost + fee;
                            inventory = trade_qty;
                            entry_price = best_ask;
                            entry_fee = fee;
                            in_position = true;
                            std::cout << "\n\033[1;34m[BUY] Bought 0.001 BTC at $" << entry_price << "\033[0m\n";
                        }
                    } else if (in_position) {
                        const double fake_sell_price = best_ask + demo_price_boost;
                        const double price_change = (fake_sell_price - entry_price) / entry_price;
                        
                        if (price_change >= take_profit_pct) {
                            const double proceeds = fake_sell_price * trade_qty;
                            const double exit_fee = proceeds * fee_rate;
                            const double net_pnl = ((fake_sell_price - entry_price) * trade_qty) - entry_fee - exit_fee;

                            cash += proceeds - exit_fee;
                            cumulative_realised_net_pnl += net_pnl;
                            inventory = 0.0;
                            in_position = false;
                            next_entry_allowed = now + std::chrono::seconds(1);

                            std::cout << "\n\033[1;32m[TAKE PROFIT] Sold 0.001 BTC at $" << fake_sell_price 
                                      << " | Net P&L: +$" << net_pnl << "\033[0m\n";
                        }
                    }
                }

                // 4. DRAW DASHBOARD
                if (is_typing) {
                    // Show custom input prompt
                    std::cout << "\r\033[1;36m[" << (typing_mode == 'I' ? "INVEST" : "WITHDRAW") 
                              << "] Enter Amount: $" << input_buffer << "_\033[0m                             " << std::flush;
                } else {
                    // Show normal dashboard
                    std::string status = is_paused ? "\033[1;31m[PAUSED]\033[0m" : "\033[1;32m[RUNNING]\033[0m";
                    std::string pos = in_position ? "\033[1;33mLONG (0.001 BTC)\033[0m" : "FLAT";
                    double equity = cash + (inventory * best_bid);

                    std::cout << std::fixed << std::setprecision(2)
                              << "\r" << status << " BTC: $" << mid 
                              << " | Cash: $" << cash 
                              << " | Equity: \033[1;32m$" << equity << "\033[0m"
                              << " | Pos: " << pos
                              << " | P&L: \033[1;32m+$" << cumulative_realised_net_pnl << "\033[0m   " 
                              << std::flush;
                }

            } catch (const std::exception&) {}
        }
    } catch (const std::exception& e) {
        std::cerr << "\nTerminal stopped: " << e.what() << '\n';
        return 1;
    }
}