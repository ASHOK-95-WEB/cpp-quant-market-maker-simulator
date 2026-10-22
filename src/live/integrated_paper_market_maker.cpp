#include "local_order_book.hpp"
#include "paper/pnl_risk_engine.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>
#include <openssl/ssl.h>
#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace net = boost::asio;
namespace ssl = net::ssl;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = net::ip::tcp;
using json = nlohmann::json;

namespace {
constexpr const char* kWsHost = "data-stream.binance.vision";
constexpr const char* kWsPort = "443";
constexpr const char* kWsTarget = "/ws/btcusdt@depth@100ms";
constexpr const char* kRestUrl =
    "https://data-api.binance.vision/api/v3/depth?symbol=BTCUSDT&limit=1000";

// Paper-only assumptions. These are explicit research parameters, not exchange quotes.
constexpr double kInitialCashUsd = 10000.0;
constexpr double kIllustrativeFeeRate = 0.001;       // 0.10% per simulated fill
constexpr double kOrderQtyBtc = 0.001;
constexpr double kMaxLongInventoryBtc = 0.005;
constexpr double kMaxLossUsd = 25.0;
constexpr double kExtraEdgeRate = 0.00015;            // 1.5 bps above assumed fees
constexpr double kMaxFairValueBiasRate = 0.00020;     // cap imbalance/microprice bias at 2 bps
constexpr double kRequoteThresholdRate = 0.00020;     // preserve a virtual quote unless it drifts > 2 bps
constexpr std::int64_t kFeedFreshMs = 2000;
constexpr std::size_t kInitialBufferedEvents = 20;

struct Snapshot {
    std::uint64_t last_update_id{};
    std::vector<PriceLevelUpdate> bids;
    std::vector<PriceLevelUpdate> asks;
};

struct ReceivedMessage {
    json payload;
    std::int64_t local_receive_ms{};
};

struct WorkingOrder {
    double price{0.0};
    double quantity{0.0};
    bool active{false};
};

struct SimulatedFill {
    std::string side{"NONE"};
    double price{0.0};
    double quantity{0.0};
};

std::int64_t system_time_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

size_t curl_write_callback(char* data, size_t size, size_t count, void* user_data) {
    const size_t bytes = size * count;
    static_cast<std::string*>(user_data)->append(data, bytes);
    return bytes;
}

std::string http_get(const std::string& url) {
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("curl_easy_init failed");

    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 15000L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "cpp-quant-lob-paper/0.2");

    const CURLcode result = curl_easy_perform(curl);
    long response_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    curl_easy_cleanup(curl);

    if (result != CURLE_OK)
        throw std::runtime_error(std::string("REST request failed: ") + curl_easy_strerror(result));
    if (response_code < 200 || response_code >= 300)
        throw std::runtime_error("REST endpoint returned HTTP " + std::to_string(response_code));
    return body;
}

double json_number(const json& value) {
    if (value.is_string()) return std::stod(value.get<std::string>());
    if (value.is_number()) return value.get<double>();
    throw std::runtime_error("Expected numeric market-data field");
}

std::vector<PriceLevelUpdate> parse_levels(const json& values) {
    std::vector<PriceLevelUpdate> result;
    result.reserve(values.size());
    for (const auto& level : values) {
        if (!level.is_array() || level.size() < 2) continue;
        result.push_back({json_number(level.at(0)), json_number(level.at(1))});
    }
    return result;
}

Snapshot fetch_snapshot() {
    const json response = json::parse(http_get(kRestUrl));
    Snapshot snapshot;
    snapshot.last_update_id = response.at("lastUpdateId").get<std::uint64_t>();
    snapshot.bids = parse_levels(response.at("bids"));
    snapshot.asks = parse_levels(response.at("asks"));
    if (snapshot.bids.empty() || snapshot.asks.empty())
        throw std::runtime_error("REST snapshot has no two-sided depth");
    return snapshot;
}

bool is_depth_event(const json& value) {
    return value.is_object() && value.value("e", std::string{}) == "depthUpdate" &&
           value.contains("U") && value.contains("u") &&
           value.contains("b") && value.contains("a");
}

DepthEvent parse_event(const json& value) {
    DepthEvent event;
    event.first_update_id = value.at("U").get<std::uint64_t>();
    event.final_update_id = value.at("u").get<std::uint64_t>();
    event.event_time_ms = value.value("E", std::int64_t{0});
    if (event.final_update_id < event.first_update_id)
        throw std::runtime_error("Depth event has invalid update-ID range");
    event.bids = parse_levels(value.at("b"));
    event.asks = parse_levels(value.at("a"));
    return event;
}

ReceivedMessage read_message(
    websocket::stream<beast::ssl_stream<beast::tcp_stream>>& ws,
    beast::flat_buffer& buffer) {
    buffer.consume(buffer.size());
    ws.read(buffer);
    // Timestamp directly after the complete WebSocket frame arrives, before JSON parsing.
    const auto local_ms = system_time_ms();
    json payload = json::parse(beast::buffers_to_string(buffer.data()));
    return ReceivedMessage{std::move(payload), local_ms};
}

double round_to_cent(double price) {
    return std::round(price * 100.0) / 100.0;
}

// Order-book-only touch model: intentionally an approximation, not a queue-position simulator.
SimulatedFill try_simulated_fill(const LocalOrderBook& book,
                                 WorkingOrder& bid_order,
                                 WorkingOrder& ask_order,
                                 paper::Ledger& ledger) {
    SimulatedFill fill;
    if (!book.valid_market()) return fill;

    // A bid fill is inferred when the visible best bid touches/goes below our resting limit.
    // An ask fill is inferred when the visible best ask touches/goes above our resting limit.
    // Depth alone cannot prove a specific queue fill; this assumption is disclosed in the README.
    if (bid_order.active && book.best_bid() <= bid_order.price) {
        const double qty = bid_order.quantity;
        const double fee_cost = bid_order.price * qty * (1.0 + ledger.fee_rate());
        if (ledger.inventory() + qty <= kMaxLongInventoryBtc + 1e-12 &&
            ledger.cash() + 1e-9 >= fee_cost) {
            ledger.apply_fill(paper::Side::Buy, bid_order.price, qty);
            fill = {"BUY", bid_order.price, qty};
        }
        bid_order.active = false;
    }

    if (fill.side == "NONE" && ask_order.active &&
        book.best_ask() >= ask_order.price && ledger.inventory() + 1e-12 >= ask_order.quantity) {
        const double qty = ask_order.quantity;
        ledger.apply_fill(paper::Side::Sell, ask_order.price, qty);
        fill = {"SELL", ask_order.price, qty};
        ask_order.active = false;
    }
    return fill;
}

void run_one_session(paper::Ledger& ledger, paper::RiskManager& risk, std::ofstream& csv) {
    net::io_context ioc;
    ssl::context tls_context(ssl::context::tls_client);
    tls_context.set_default_verify_paths();
    tls_context.set_verify_mode(ssl::verify_peer);
    tcp::resolver resolver(ioc);
    websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws(ioc, tls_context);
    const auto endpoints = resolver.resolve(kWsHost, kWsPort);
    beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(20));
    beast::get_lowest_layer(ws).connect(endpoints);
    if (SSL_set_tlsext_host_name(ws.next_layer().native_handle(), kWsHost) != 1)
        throw std::runtime_error("Failed to set TLS SNI");
    ws.next_layer().set_verify_callback(ssl::host_name_verification(kWsHost));
    ws.next_layer().handshake(ssl::stream_base::client);
    ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
    ws.handshake(kWsHost, kWsTarget);
    beast::get_lowest_layer(ws).expires_never();
    beast::flat_buffer ws_buffer;

    std::cout << "Connected to public Binance incremental depth stream; buffering for snapshot sync...\n";
    std::vector<ReceivedMessage> buffered;
    buffered.reserve(kInitialBufferedEvents);
    while (buffered.size() < kInitialBufferedEvents) {
        try {
            ReceivedMessage received = read_message(ws, ws_buffer);
            if (is_depth_event(received.payload)) buffered.push_back(std::move(received));
        } catch (const json::exception&) {
            // Ignore malformed/non-depth payloads; the stream is public and read-only.
        }
    }

    const Snapshot snapshot = fetch_snapshot();
    LocalOrderBook book;
    book.load_snapshot(snapshot.last_update_id, snapshot.bids, snapshot.asks);

    WorkingOrder working_bid;
    WorkingOrder working_ask;
    std::size_t printed = 0;
    std::size_t applied_count = 0;
    double last_valid_mark_bid = book.valid_market() ? book.best_bid() : 1.0;

    auto process_event = [&](const ReceivedMessage& received, const DepthEvent& event) {
        const std::int64_t now_ms = system_time_ms();
        const bool feed_fresh = received.local_receive_ms > 0 &&
            now_ms >= received.local_receive_ms &&
            (now_ms - received.local_receive_ms) <= kFeedFreshMs;

        SimulatedFill fill;
        std::string risk_status = "OK";
        if (book.valid_market()) last_valid_mark_bid = book.best_bid();
        double fair_value = 0.0;
        double desired_bid = 0.0;
        double desired_ask = 0.0;

        if (!feed_fresh) {
            working_bid.active = false;
            working_ask.active = false;
            risk_status = "STALE_FEED_QUOTES_CANCELLED";
        } else if (!book.valid_market()) {
            working_bid.active = false;
            working_ask.active = false;
            risk_status = "INVALID_BOOK_QUOTES_CANCELLED";
        } else {
            fill = try_simulated_fill(book, working_bid, working_ask, ledger);

            const double mid = book.mid_price();
            const double imbalance = book.depth_imbalance(5);
            const double raw_bias = (book.microprice() - mid) + imbalance * mid * 0.0001;
            const double max_bias = mid * kMaxFairValueBiasRate;
            const double bias = std::clamp(raw_bias, -max_bias, max_bias);
            fair_value = mid + bias;
            const double half_width = mid * (ledger.fee_rate() + kExtraEdgeRate);
            desired_bid = round_to_cent(fair_value - half_width);
            desired_ask = round_to_cent(fair_value + half_width);

            const paper::MarketState market{book.best_bid(), book.best_ask(), true, feed_fresh};
            const paper::OrderIntent bid_intent{paper::Side::Buy, desired_bid, kOrderQtyBtc};
            const paper::RiskDecision bid_decision = risk.check(bid_intent, market, ledger);
            if (risk.halted()) {
                working_bid.active = false;
                working_ask.active = false;
                risk_status = "KILL_SWITCH_MAX_LOSS";
            } else {
                const double reprice_threshold = mid * kRequoteThresholdRate;
                if (bid_decision.allowed) {
                    if (!working_bid.active ||
                        std::abs(working_bid.price - desired_bid) > reprice_threshold) {
                        working_bid = WorkingOrder{desired_bid, kOrderQtyBtc, true};
                    }
                } else {
                    working_bid.active = false;
                    risk_status = "BID_REJECTED_" + bid_decision.reason;
                }

                if (ledger.inventory() + 1e-12 >= kOrderQtyBtc) {
                    const paper::OrderIntent ask_intent{
                        paper::Side::Sell, desired_ask,
                        std::min(kOrderQtyBtc, ledger.inventory())};
                    const paper::RiskDecision ask_decision = risk.check(ask_intent, market, ledger);
                    if (risk.halted()) {
                        working_bid.active = false;
                        working_ask.active = false;
                        risk_status = "KILL_SWITCH_MAX_LOSS";
                    } else if (ask_decision.allowed) {
                        const double reprice_threshold = mid * kRequoteThresholdRate;
                        if (!working_ask.active ||
                            std::abs(working_ask.price - desired_ask) > reprice_threshold ||
                            working_ask.quantity > ledger.inventory() + 1e-12) {
                            working_ask = WorkingOrder{
                                desired_ask, std::min(kOrderQtyBtc, ledger.inventory()), true};
                        }
                    } else {
                        working_ask.active = false;
                        if (risk_status == "OK")
                            risk_status = "ASK_REJECTED_" + ask_decision.reason;
                    }
                } else {
                    working_ask.active = false;
                }
            }
        }

        if (risk.halted()) {
            working_bid.active = false;
            working_ask.active = false;
            risk_status = "KILL_SWITCH_MAX_LOSS";
        }

        const paper::PortfolioMark mark = ledger.mark_to_market(last_valid_mark_bid);

        csv << received.local_receive_ms << ',' << event.event_time_ms << ','
            << book.last_update_id() << ',';
        if (book.valid_market()) {
            csv << std::setprecision(12)
                << book.best_bid() << ',' << book.best_bid_qty() << ','
                << book.best_ask() << ',' << book.best_ask_qty() << ','
                << book.spread() << ',' << book.mid_price() << ',' << book.microprice() << ','
                << book.depth_imbalance(5) << ',';
        } else {
            csv << "0,0,0,0,0,0,0,0,";
        }
        csv << fair_value << ',' << desired_bid << ','
            << (working_bid.active ? working_bid.price : 0.0) << ','
            << desired_ask << ',' << (working_ask.active ? working_ask.price : 0.0) << ','
            << mark.cash << ',' << mark.inventory << ',' << mark.net_pnl << ','
            << mark.realised_net_pnl << ',' << mark.fees_paid << ',' << mark.current_drawdown << ','
            << fill.side << ',' << fill.price << ',' << fill.quantity << ','
            << std::quoted(risk_status) << '\n';

        if (++applied_count % 20 == 0) csv.flush();
        if (++printed % 10 == 0) {
            std::cout << std::fixed << std::setprecision(2) << "BTC ";
            if (book.valid_market()) {
                std::cout << "bid/ask " << book.best_bid() << "/" << book.best_ask()
                          << " | imb5 " << std::setprecision(3) << book.depth_imbalance(5);
            } else {
                std::cout << "INVALID BOOK";
            }
            std::cout << " | quote " << std::setprecision(2)
                      << (working_bid.active ? working_bid.price : 0.0) << "/"
                      << (working_ask.active ? working_ask.price : 0.0)
                      << " | inv " << std::setprecision(4) << mark.inventory
                      << " | net P&L $" << std::setprecision(4) << mark.net_pnl
                      << " | realised $" << mark.realised_net_pnl
                      << " | fees $" << mark.fees_paid
                      << " | drawdown $" << mark.current_drawdown
                      << " | " << risk_status;
            if (fill.side != "NONE")
                std::cout << " | PAPER FILL " << fill.side << ' ' << fill.quantity
                          << " BTC @ " << std::setprecision(2) << fill.price;
            std::cout << '\n';
        }
    };

    std::cout << "Snapshot loaded at update ID " << snapshot.last_update_id << ".\n"
              << "Seeking first event that bridges snapshot update ID...\n";
    bool synchronized = false;
    auto apply_received = [&](const ReceivedMessage& received) {
        if (!is_depth_event(received.payload)) return;
        const DepthEvent event = parse_event(received.payload);
        const ApplyResult result = book.apply(event);
        if (result == ApplyResult::GapDetected)
            throw std::runtime_error("Depth update gap/failed snapshot bridge; rebuilding the book");
        if (result == ApplyResult::Applied) {
            synchronized = true;
            process_event(received, event);
        }
    };

    // Skip updates covered by the REST snapshot; the first new update must bridge lastUpdateId+1.
    for (const auto& received : buffered) {
        apply_received(received);
        if (synchronized) break;
    }
    // If snapshot was newer than all buffered events, read until a valid bridge appears.
    while (!synchronized) {
        ReceivedMessage received;
        try {
            received = read_message(ws, ws_buffer);
        } catch (const json::exception&) {
            continue;
        }
        apply_received(received);
    }

    std::cout << "SYNCHRONIZED local book is live. Logging to data/integrated_paper_trader.csv.\n"
              << "Paper only: no API keys, accounts, or real orders. Ctrl+C to stop.\n";

    // Replay the remainder of the initial buffer in sequence order, then continue live.
    // The first bridge was consumed above, so only messages still newer than the book are applied.
    for (const auto& received : buffered) {
        if (!is_depth_event(received.payload)) continue;
        const DepthEvent event = parse_event(received.payload);
        if (event.final_update_id <= book.last_update_id()) continue;
        const ApplyResult result = book.apply(event);
        if (result == ApplyResult::GapDetected)
            throw std::runtime_error("Gap while replaying buffered events; rebuilding the book");
        if (result == ApplyResult::Applied) process_event(received, event);
    }

    while (true) {
        ReceivedMessage received;
        try {
            received = read_message(ws, ws_buffer);
        } catch (const json::exception&) {
            continue;
        }
        if (!is_depth_event(received.payload)) continue;
        const DepthEvent event = parse_event(received.payload);
        const ApplyResult result = book.apply(event);
        if (result == ApplyResult::GapDetected)
            throw std::runtime_error("Live depth update gap detected; rebuilding the book");
        if (result == ApplyResult::IgnoredOld) continue;
        process_event(received, event);
    }
}

} // namespace

int main() {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        std::cerr << "Failed to initialize libcurl.\n";
        return 1;
    }

    std::filesystem::create_directories("data");
    const std::string csv_path = "data/integrated_paper_trader.csv";
    const bool needs_header = !std::filesystem::exists(csv_path) ||
                              std::filesystem::file_size(csv_path) == 0;
    std::ofstream csv(csv_path, std::ios::app);
    if (!csv) {
        std::cerr << "Cannot open " << csv_path << '\n';
        curl_global_cleanup();
        return 1;
    }
    if (needs_header) {
        csv << "local_receive_ms,event_time_ms,update_id,best_bid,best_bid_qty,best_ask,best_ask_qty,"
               "spread,mid_price,microprice,depth_imbalance_5,fair_value,desired_bid,working_bid,"
               "desired_ask,working_ask,cash_usd,inventory_btc,net_pnl_usd,realised_net_pnl_usd,"
               "fees_paid_usd,current_drawdown_usd,fill_side,fill_price,fill_qty,risk_status\n";
        csv.flush();
    }

    try {
        paper::Ledger ledger(kInitialCashUsd, kIllustrativeFeeRate);
        paper::RiskConfig config;
        config.max_order_quantity = kOrderQtyBtc;
        config.max_long_inventory = kMaxLongInventoryBtc;
        config.max_loss_usd = kMaxLossUsd;
        paper::RiskManager risk(config);

        std::cout << "Integrated BTC/USDT paper market maker\n"
                  << "Public Binance market data only; no account credentials or real orders.\n"
                  << "Initial virtual cash: $" << kInitialCashUsd
                  << " | fee assumption: " << (kIllustrativeFeeRate * 100.0) << "% per fill\n"
                  << "Order quantity: " << kOrderQtyBtc << " BTC"
                  << " | max inventory: " << kMaxLongInventoryBtc << " BTC"
                  << " | max loss: $" << kMaxLossUsd << "\n"
                  << "Quotes target assumed round-trip fees plus an extra edge; fill detection is a simplified book-touch model.\n";

        while (true) {
            try {
                run_one_session(ledger, risk, csv);
            } catch (const std::exception& e) {
                csv.flush();
                std::cerr << "Feed/session reset: " << e.what()
                          << "\nReconnecting and rebuilding the local book in 2 seconds...\n";
                std::this_thread::sleep_for(std::chrono::seconds(2));
                if (risk.halted()) {
                    std::cerr << "Risk kill switch is latched; process will reconnect for observation only.\n";
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Fatal initialization error: " << e.what() << '\n';
        csv.close();
        curl_global_cleanup();
        return 1;
    }

    // The long-running loop normally exits only by user interruption.
    csv.flush();
    csv.close();
    curl_global_cleanup();
    return 0;
}
