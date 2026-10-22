#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>
#include <openssl/ssl.h>

#include <algorithm>
#include <chrono>
#include <cmath>
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

static double round_to_cent(double value) {
    return std::round(value * 100.0) / 100.0;
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

        // PAPER-TRADING ONLY. No order-placement API is called by this program.
        constexpr double initial_cash = 100000.0;
        constexpr double fee_rate = 0.001;       // Illustrative 0.1% fee per fill; verify real fees separately.
        constexpr double order_qty = 0.01;       // BTC per simulated fill.
        constexpr double max_inventory = 0.05;   // Hard long-inventory limit.
        constexpr double extra_edge_rate = 0.0005; // Additional 0.05% half-spread beyond the fee allowance.
        constexpr double inventory_skew_rate = 0.0005; // Shift quote center down as long inventory grows.
        constexpr auto reprice_after = std::chrono::seconds(5);

        double cash = initial_cash;
        double inventory = 0.0;
        double average_cost_per_btc = 0.0;       // Includes buy fees allocated to inventory.
        double realised_net_pnl = 0.0;
        double fees_paid = 0.0;

        bool bid_order_active = false;
        bool ask_order_active = false;
        double resting_bid = 0.0;
        double resting_ask = 0.0;
        auto bid_created = SteadyClock::now();
        auto ask_created = SteadyClock::now();

        beast::flat_buffer buffer;
        std::cout << "Connected to Binance. Starting PAPER MARKET MAKER...\n"
                  << "Instrument: BTC/USDT | Initial virtual cash: $" << initial_cash
                  << " | Simulated fee rate: " << fee_rate * 100.0 << "% per fill\n"
                  << "No live orders will be submitted. Press Ctrl+C to stop.\n\n";
        std::cout << std::fixed << std::setprecision(2);

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
                const double best_bid_qty = level_number(bids.at(0), 1);
                const double best_ask = level_number(asks.at(0), 0);
                const double best_ask_qty = level_number(asks.at(0), 1);
                if (best_bid <= 0.0 || best_ask <= 0.0 || best_bid > best_ask ||
                    best_bid_qty < 0.0 || best_ask_qty < 0.0 ||
                    best_bid_qty + best_ask_qty <= 0.0) {
                    continue;
                }

                const double mid = (best_bid + best_ask) / 2.0;
                const double imbalance = (best_bid_qty - best_ask_qty) /
                                         (best_bid_qty + best_ask_qty);
                const auto now = SteadyClock::now();

                // Fills are heuristic simulations: a resting virtual quote fills only if
                // the observed opposite best quote crosses it. Depth snapshots cannot
                // reveal our exact queue position or guarantee an exchange-like fill.
                if (bid_order_active && best_ask <= resting_bid &&
                    inventory + order_qty <= max_inventory + 1e-12) {
                    const double cost = resting_bid * order_qty;
                    const double fee = cost * fee_rate;
                    if (cash >= cost + fee) {
                        const double new_inventory = inventory + order_qty;
                        average_cost_per_btc =
                            (inventory * average_cost_per_btc + cost + fee) / new_inventory;
                        cash -= cost + fee;
                        inventory = new_inventory;
                        fees_paid += fee;
                        std::cout << "\n[PAPER FILL] BOUGHT " << order_qty << " BTC at $"
                                  << resting_bid << " | Fee: $" << fee << "\n";
                    }
                    bid_order_active = false;
                }

                if (ask_order_active && best_bid >= resting_ask &&
                    inventory + 1e-12 >= order_qty) {
                    const double proceeds = resting_ask * order_qty;
                    const double fee = proceeds * fee_rate;
                    const double net_proceeds = proceeds - fee;
                    const double cost_basis = average_cost_per_btc * order_qty;
                    const double trade_net = net_proceeds - cost_basis;
                    cash += net_proceeds;
                    inventory -= order_qty;
                    realised_net_pnl += trade_net;
                    fees_paid += fee;
                    if (inventory < 1e-10) {
                        inventory = 0.0;
                        average_cost_per_btc = 0.0;
                    }
                    std::cout << "\n[PAPER FILL] SOLD " << order_qty << " BTC at $"
                              << resting_ask << " | Exit fee: $" << fee
                              << " | Realised net P&L: $" << trade_net << "\n";
                    ask_order_active = false;
                }

                // Replace stale unfilled virtual quotes no more frequently than every 5 sec.
                // This avoids recomputing a new quote from the same snapshot on every update.
                if (bid_order_active && now - bid_created >= reprice_after) {
                    bid_order_active = false;
                }
                if (ask_order_active && now - ask_created >= reprice_after) {
                    ask_order_active = false;
                }

                if (!bid_order_active && inventory + order_qty <= max_inventory + 1e-12) {
                    const double half_spread = mid * (fee_rate + extra_edge_rate);
                    const double inventory_ratio = std::clamp(inventory / max_inventory, 0.0, 1.0);
                    const double quote_center = mid - inventory_ratio * mid * inventory_skew_rate;
                    const double proposed_bid = round_to_cent(quote_center - half_spread);
                    const double required_cash = proposed_bid * order_qty * (1.0 + fee_rate);
                    if (proposed_bid > 0.0 && cash >= required_cash) {
                        resting_bid = proposed_bid;
                        bid_created = now;
                        bid_order_active = true;
                    }
                }

                if (!ask_order_active && inventory + 1e-12 >= order_qty) {
                    const double half_spread = mid * (fee_rate + extra_edge_rate);
                    const double inventory_ratio = std::clamp(inventory / max_inventory, 0.0, 1.0);
                    const double quote_center = mid - inventory_ratio * mid * inventory_skew_rate;
                    const double proposed_ask = round_to_cent(quote_center + half_spread);
                    if (proposed_ask > 0.0) {
                        resting_ask = proposed_ask;
                        ask_created = now;
                        ask_order_active = true;
                    }
                }

                // Mark long inventory at best bid (a conservative liquidation-side mark).
                const double marked_equity_before_exit_fee = cash + inventory * best_bid;
                const double estimated_exit_fee = inventory * best_bid * fee_rate;
                const double estimated_liquidation_equity =
                    marked_equity_before_exit_fee - estimated_exit_fee;
                const double estimated_net_pnl = estimated_liquidation_equity - initial_cash;
                const double unrealised_net_pnl = estimated_net_pnl - realised_net_pnl;

                std::cout << "BTC bid/ask: $" << best_bid << " / $" << best_ask
                          << " | Imbalance: " << std::setprecision(3) << imbalance
                          << " | Cash: $" << std::setprecision(2) << cash
                          << " | Inv: " << std::setprecision(4) << inventory
                          << " BTC | Realised net: $" << std::setprecision(4) << realised_net_pnl
                          << " | Unrealised net est.: $" << unrealised_net_pnl
                          << " | Net P&L est.: $" << estimated_net_pnl
                          << " | Fees paid: $" << fees_paid
                          << " | Bid quote: ";
                if (bid_order_active) std::cout << resting_bid; else std::cout << "--";
                std::cout << " | Ask quote: ";
                if (ask_order_active) std::cout << resting_ask; else std::cout << "--";
                std::cout << "   \r" << std::flush;
            } catch (const json::exception&) {
                // Binance can send non-depth JSON messages; ignore those safely.
            } catch (const std::exception& e) {
                std::cerr << "\nSkipping invalid market snapshot: " << e.what() << '\n';
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "\nPaper market maker stopped: " << e.what() << '\n';
        return 1;
    }
}
