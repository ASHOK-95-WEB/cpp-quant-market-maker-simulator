#include "local_order_book.hpp"
#include "paper/pnl_risk_engine.hpp"
#include "paper/trade_print_fill_model.hpp"

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
constexpr const char* kWsTarget = "/stream?streams=btcusdt@depth@100ms/btcusdt@aggTrade";
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
    double quantity{0.0}; // remaining quantity
    bool active{false};
    double queue_ahead_quantity{0.0}; // estimated displayed quantity at this exact price
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

bool is_agg_trade_event(const json& value) {
    return value.is_object() && value.value("e", std::string{}) == "aggTrade" &&
           value.contains("p") && value.contains("q") && value.contains("m");
}

paper::TradePrint parse_trade_print(const json& value) {
    const bool buyer_is_maker = value.at("m").get<bool>();
    const paper::AggressorSide aggressor = buyer_is_maker
        ? paper::AggressorSide::Sell
        : paper::AggressorSide::Buy;
    return paper::TradePrint{
        json_number(value.at("p")), json_number(value.at("q")), aggressor};
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
    // Combined Binance streams wrap each raw event in {"stream":"...","data":{...}}.
    if (payload.is_object() && payload.contains("data") && payload.at("data").is_object())
        payload = payload.at("data");
    return ReceivedMessage{std::move(payload), local_ms};
}

double round_to_cent(double price) {
    return std::round(price * 100.0) / 100.0;
}

paper::RestingOrder to_model_order(const WorkingOrder& order, paper::RestingSide side) {
    return paper::RestingOrder{side, order.price, order.quantity, order.quantity,
                               order.queue_ahead_quantity, 0.0, order.active};
}

void store_model_order(WorkingOrder& target, const paper::RestingOrder& source) {
    target.quantity = source.remaining_quantity;
    target.queue_ahead_quantity = source.queue_ahead_quantity;
    target.active = source.active;
}

// Fills are inferred only from public aggTrade prints at the hypothetical limit price.
// Same-price displayed depth is an estimate of queue ahead, not exchange queue certainty.
SimulatedFill try_trade_print_fill(const paper::TradePrint& trade,
                                   const LocalOrderBook& book,
                                   WorkingOrder& bid_order,
                                   WorkingOrder& ask_order,
                                   paper::Ledger& ledger,
                                   paper::RiskManager& risk,
                                   bool feed_fresh,
                                   std::string& risk_status) {
    if (risk.halted()) {
        bid_order.active = false;
        ask_order.active = false;
        risk_status = "KILL_SWITCH_MAX_LOSS";
        return {};
    }
    if (!feed_fresh) {
        bid_order.active = false;
        ask_order.active = false;
        risk_status = "STALE_FEED_QUOTES_CANCELLED";
        return {};
    }
    if (!book.valid_market()) {
        bid_order.active = false;
        ask_order.active = false;
        risk_status = "INVALID_BOOK_QUOTES_CANCELLED";
        return {};
    }

    const paper::MarketState market{
        book.best_bid(), book.best_ask(), true, feed_fresh};
    auto attempt = [&](WorkingOrder& working,
                       paper::RestingSide resting_side,
                       paper::Side ledger_side) -> SimulatedFill {
        if (!working.active) return {};

        paper::RestingOrder model = to_model_order(working, resting_side);
        const paper::FillDelta delta = paper::apply_trade_print(model, trade, 0.001);
        if (!delta) {
            // Commit any queue-ahead depletion even if this print did not fill us.
            store_model_order(working, model);
            return {};
        }

        const paper::OrderIntent fill_intent{ledger_side, delta.price, delta.quantity};
        const paper::RiskDecision decision = risk.check(fill_intent, market, ledger);
        if (risk.halted()) {
            working.active = false;
            bid_order.active = false;
            ask_order.active = false;
            risk_status = "KILL_SWITCH_MAX_LOSS";
            return {};
        }
        if (!decision.allowed) {
            working.active = false;
            risk_status = "TRADE_FILL_REJECTED_" + decision.reason;
            return {};
        }

        store_model_order(working, model);
        ledger.apply_fill(ledger_side, delta.price, delta.quantity);
        risk_status = "TRADE_PRINT_FILL";
        return SimulatedFill{
            ledger_side == paper::Side::Buy ? "BUY" : "SELL",
            delta.price, delta.quantity};
    };

    // Binance aggTrade m=true means buyer was maker, so the aggressor was a seller.
    if (trade.aggressor == paper::AggressorSide::Sell)
        return attempt(bid_order, paper::RestingSide::Buy, paper::Side::Buy);
    return attempt(ask_order, paper::RestingSide::Sell, paper::Side::Sell);
}
void run_one_session(paper::Ledger& ledger, paper::RiskManager& risk, std::ofstream& csv, std::ofstream& trade_csv) {
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

    std::cout << "Connected to public Binance depth + aggTrade streams; buffering depth for snapshot sync...\n";
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
            // No depth-touch fills: a virtual order fills only after a matching public aggTrade print.

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
                        working_bid = WorkingOrder{desired_bid, kOrderQtyBtc, true, book.bid_quantity_at_price(desired_bid)};
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
                                desired_ask, std::min(kOrderQtyBtc, ledger.inventory()), true,
                                book.ask_quantity_at_price(desired_ask)};
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

    std::size_t trade_print_count = 0;
    std::size_t simulated_fill_count = 0;
    auto process_trade = [&](const ReceivedMessage& received, const json& trade_payload) {
        const paper::TradePrint trade = parse_trade_print(trade_payload);
        const bool feed_fresh = received.local_receive_ms > 0 &&
            system_time_ms() >= received.local_receive_ms &&
            (system_time_ms() - received.local_receive_ms) <= kFeedFreshMs;
        std::string trade_risk_status = "OK";
        const SimulatedFill fill = try_trade_print_fill(
            trade, book, working_bid, working_ask, ledger, risk,
            feed_fresh, trade_risk_status);
        const paper::PortfolioMark mark = ledger.mark_to_market(last_valid_mark_bid);
        const std::int64_t event_time_ms = trade_payload.value(
            "T", trade_payload.value("E", std::int64_t{0}));
        const char* aggressor = trade.aggressor == paper::AggressorSide::Buy ? "BUY" : "SELL";

        trade_csv << received.local_receive_ms << ',' << event_time_ms << ','
                  << std::setprecision(12) << trade.price << ',' << trade.quantity << ','
                  << aggressor << ','
                  << (working_bid.active ? working_bid.price : 0.0) << ','
                  << working_bid.queue_ahead_quantity << ','
                  << (working_ask.active ? working_ask.price : 0.0) << ','
                  << working_ask.queue_ahead_quantity << ','
                  << fill.side << ',' << fill.price << ',' << fill.quantity << ','
                  << mark.cash << ',' << mark.inventory << ',' << mark.net_pnl << ','
                  << mark.realised_net_pnl << ',' << mark.fees_paid << ','
                  << mark.current_drawdown << ',' << std::quoted(trade_risk_status) << '\n';

        ++trade_print_count;
        if (fill.side != "NONE") {
            ++simulated_fill_count;
            std::cout << "[TRADE-PRINT PAPER FILL] " << fill.side << ' '
                      << fill.quantity << " BTC @ " << std::fixed << std::setprecision(2)
                      << fill.price << " | inventory " << std::setprecision(4)
                      << mark.inventory << " BTC | net P&L $" << mark.net_pnl << '\n';
        }
        if (trade_print_count % 100 == 0) trade_csv.flush();
        if (trade_print_count % 1000 == 0) {
            std::cout << "Observed aggTrade prints: " << trade_print_count
                      << " | paper fills: " << simulated_fill_count << '\n';
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

    std::cout << "SYNCHRONIZED local book is live. Logging book state to data/integrated_paper_trader.csv and trade events to data/trade_print_fill_events.csv.\n"
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
        if (is_agg_trade_event(received.payload)) {
            process_trade(received, received.payload);
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

    const std::string trade_csv_path = "data/trade_print_fill_events.csv";
    const bool trade_csv_needs_header = !std::filesystem::exists(trade_csv_path) ||
        std::filesystem::file_size(trade_csv_path) == 0;
    std::ofstream trade_csv(trade_csv_path, std::ios::app);
    if (!trade_csv) {
        std::cerr << "Cannot open " << trade_csv_path << '\n';
        csv.close();
        curl_global_cleanup();
        return 1;
    }
    if (trade_csv_needs_header) {
        trade_csv << "local_receive_ms,event_time_ms,trade_price,trade_qty,aggressor_side,"
                     "working_bid_price,bid_queue_ahead_after_btc,working_ask_price,"
                     "ask_queue_ahead_after_btc,fill_side,fill_price,fill_qty,cash_usd,"
                     "inventory_btc,net_pnl_usd,realised_net_pnl_usd,fees_paid_usd,"
                     "current_drawdown_usd,risk_status\n";
        trade_csv.flush();
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
                  << "Quotes target assumed fees plus an extra edge; simulated fills require matching public aggTrade prints.\n";

        while (true) {
            try {
                run_one_session(ledger, risk, csv, trade_csv);
            } catch (const std::exception& e) {
                csv.flush();
                trade_csv.flush();
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
        trade_csv.close();
        curl_global_cleanup();
        return 1;
    }

    // The long-running loop normally exits only by user interruption.
    csv.flush();
    csv.close();
    trade_csv.flush();
    trade_csv.close();
    curl_global_cleanup();
    return 0;
}
