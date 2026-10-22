#include "local_order_book.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>
#include <openssl/ssl.h>
#include <curl/curl.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
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

struct Snapshot {
    std::uint64_t last_update_id{};
    std::vector<PriceLevelUpdate> bids;
    std::vector<PriceLevelUpdate> asks;
};

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
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "cpp-quant-lob/0.1");

    const CURLcode result = curl_easy_perform(curl);
    long response_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    curl_easy_cleanup(curl);

    if (result != CURLE_OK)
        throw std::runtime_error(std::string("REST request failed: ") + curl_easy_strerror(result));
    if (response_code < 200 || response_code >= 300)
        throw std::runtime_error("REST endpoint returned HTTP " + std::to_string(response_code) + ": " + body);
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

struct ReceivedMessage {
    json payload;
    std::int64_t local_receive_ms{};
};

ReceivedMessage read_message(
    websocket::stream<beast::ssl_stream<beast::tcp_stream>>& ws,
    beast::flat_buffer& buffer) {
    buffer.consume(buffer.size());
    ws.read(buffer);

    // Capture the local receive time immediately after the complete WebSocket
    // frame is read, before JSON parsing or other processing.
    const auto received_at = std::chrono::system_clock::now();
    const auto local_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        received_at.time_since_epoch()).count();

    json payload = json::parse(beast::buffers_to_string(buffer.data()));
    return ReceivedMessage{std::move(payload), local_ms};
}

void print_and_log(const LocalOrderBook& book, std::int64_t local_ms,
                   std::int64_t event_ms, std::ofstream& csv, unsigned& rows_since_flush) {
    if (!book.valid_market()) {
        std::cerr << "Book invalid/crossed; skipping metrics.\n";
        return;
    }

    const double imbalance = book.depth_imbalance(5);
    const double spread = book.spread();
    const double mid = book.mid_price();
    const double micro = book.microprice();
    const auto update_id = book.last_update_id();

    std::cout << std::fixed << std::setprecision(8)
              << "update=" << update_id
              << " bid=" << book.best_bid() << " x " << book.best_bid_qty()
              << " ask=" << book.best_ask() << " x " << book.best_ask_qty()
              << " spread=" << spread
              << " mid=" << mid
              << " microprice=" << micro
              << " depth_imbalance_5=" << std::setprecision(5) << imbalance
              << "\n";

    csv << local_ms << ',' << event_ms << ',' << update_id << ','
        << std::setprecision(12) << book.best_bid() << ',' << book.best_bid_qty() << ','
        << book.best_ask() << ',' << book.best_ask_qty() << ','
        << spread << ',' << mid << ',' << micro << ',' << imbalance << '\n';
    if (++rows_since_flush >= 20) {
        csv.flush();
        rows_since_flush = 0;
    }
}

void run_one_session() {
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

    std::cout << "Connected to Binance incremental depth stream.\n"
              << "Buffering updates before REST snapshot...\n";

    // Start the WebSocket first, then buffer events while obtaining a snapshot.
    constexpr std::size_t initial_buffer_events = 20;
    std::vector<ReceivedMessage> buffered;
    buffered.reserve(initial_buffer_events);
    beast::flat_buffer ws_buffer;
    while (buffered.size() < initial_buffer_events) {
        try {
            ReceivedMessage received = read_message(ws, ws_buffer);
            if (is_depth_event(received.payload))
                buffered.push_back(std::move(received));
        } catch (const json::exception&) {
            // Ignore non-JSON/control payloads; Beast handles WebSocket control frames.
        }
    }

    Snapshot snapshot;
    bool snapshot_is_new_enough = false;
    for (int attempt = 0; attempt < 5; ++attempt) {
        snapshot = fetch_snapshot();
        if (!buffered.empty() &&
            snapshot.last_update_id >=
                buffered.front().payload.at("U").get<std::uint64_t>()) {
            snapshot_is_new_enough = true;
            break;
        }
        std::cerr << "Snapshot is older than the first buffered event; retrying snapshot...\n";
    }
    if (!snapshot_is_new_enough)
        throw std::runtime_error("Could not obtain a snapshot recent enough to synchronize");

    LocalOrderBook book;
    book.load_snapshot(snapshot.last_update_id, snapshot.bids, snapshot.asks);

    std::filesystem::create_directories("data");
    const std::string csv_path = "data/binance_local_book.csv";
    const bool needs_header = !std::filesystem::exists(csv_path) ||
                              std::filesystem::file_size(csv_path) == 0;
    std::ofstream csv(csv_path, std::ios::app);
    if (!csv) throw std::runtime_error("Cannot open " + csv_path);
    if (needs_header) {
        csv << "local_receive_ms,event_time_ms,update_id,best_bid,best_bid_qty,"
               "best_ask,best_ask_qty,spread,mid_price,microprice,depth_imbalance_5\n";
        csv.flush();
    }

    std::cout << "Snapshot loaded at update ID " << book.last_update_id() << ".\n"
              << "Local book ready: BTC/USDT. Logging to " << csv_path << "\n"
              << "Press Ctrl+C to stop.\n";
    unsigned rows_since_flush = 0;

    // Apply buffered events in their original order (old events are ignored).
    for (const auto& received : buffered) {
        const auto event = parse_event(received.payload);
        if (event.final_update_id <= snapshot.last_update_id) continue;
        const auto result = book.apply(event);
        if (result == ApplyResult::GapDetected)
            throw std::runtime_error("Gap detected while applying initial buffer; resynchronizing");
        if (result == ApplyResult::Applied) {
            print_and_log(book, received.local_receive_ms, event.event_time_ms,
                          csv, rows_since_flush);
        }
    }

    while (true) {
        ReceivedMessage received;
        try {
            received = read_message(ws, ws_buffer);
        } catch (const json::exception&) {
            continue;
        }
        if (!is_depth_event(received.payload)) continue;

        const auto event = parse_event(received.payload);
        const auto result = book.apply(event);
        if (result == ApplyResult::GapDetected) {
            csv.flush();
            throw std::runtime_error("Depth update gap detected; local book must be rebuilt");
        }
        if (result == ApplyResult::IgnoredOld) continue;

        print_and_log(book, received.local_receive_ms, event.event_time_ms,
                      csv, rows_since_flush);
    }
}
} // namespace

int main() {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        std::cerr << "Failed to initialise libcurl.\n";
        return 1;
    }

    std::cout << "Binance local order-book monitor (public data only).\n"
              << "No account credentials or trading orders are used.\n";

    while (true) {
        try {
            run_one_session();
        } catch (const std::exception& e) {
            std::cerr << "Feed session ended: " << e.what() << "\n"
                      << "Waiting 2 seconds, then reconnecting and rebuilding the book...\n";
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }

    curl_global_cleanup();
    return 0;
}
