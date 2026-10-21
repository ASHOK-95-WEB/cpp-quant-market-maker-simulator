
#include <algorithm>
#include <cmath>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

constexpr double TICK = 0.01;
constexpr double MAX_SPREAD = 0.50;
constexpr double HALF_SPREAD = 0.03;
constexpr double INVENTORY_SKEW = 0.002;
constexpr long long MAX_INVENTORY = 30;
constexpr long long MAX_ORDER_SIZE = 10;
constexpr double EPS = 1e-7;

struct Market {
    long long timestamp;
    double bid;
    long long bid_qty;
    double ask;
    long long ask_qty;
    double next_bid;
    double next_ask;
};

struct Quotes {
    double bid;
    double ask;
};

bool tick_aligned(double price) {
    return std::abs(price / TICK -
                    std::round(price / TICK)) < 1e-5;
}

bool valid_market(const Market& m) {
    if (!std::isfinite(m.bid) ||
        !std::isfinite(m.ask) ||
        !std::isfinite(m.next_bid) ||
        !std::isfinite(m.next_ask)) {
        return false;
    }

    if (m.bid <= 0 || m.ask <= 0 ||
        m.next_bid <= 0 || m.next_ask <= 0) {
        return false;
    }

    if (m.bid_qty <= 0 || m.ask_qty <= 0) {
        return false;
    }

    const double spread = m.ask - m.bid;
    const double next_spread = m.next_ask - m.next_bid;

    if (spread < TICK - EPS ||
        spread > MAX_SPREAD + EPS) {
        return false;
    }

    if (next_spread < TICK - EPS ||
        next_spread > MAX_SPREAD + EPS) {
        return false;
    }

    return tick_aligned(m.bid) &&
           tick_aligned(m.ask) &&
           tick_aligned(m.next_bid) &&
           tick_aligned(m.next_ask);
}

// This sample uses event sequence numbers, not clock time.
// A gap means one or more expected snapshots are missing.
bool valid_sequence(long long current,
                    bool has_previous,
                    long long previous) {
    if (!has_previous) {
        return true;
    }

    return current > previous &&
           current - previous == 1;
}

Quotes make_quotes(const Market& m, long long inventory) {
    const double bid_qty = static_cast<double>(m.bid_qty);
    const double ask_qty = static_cast<double>(m.ask_qty);

    const double fair =
        (m.ask * bid_qty + m.bid * ask_qty) /
        (bid_qty + ask_qty);

    const double center =
        fair - INVENTORY_SKEW *
                   static_cast<double>(inventory);

    const double bid = std::clamp(
        std::round((center - HALF_SPREAD) / TICK) * TICK,
        m.bid, m.ask - TICK
    );

    const double ask = std::clamp(
        std::round((center + HALF_SPREAD) / TICK) * TICK,
        m.bid + TICK, m.ask
    );

    return {bid, ask};
}

bool valid_quotes(const Market& m, const Quotes& q) {
    return std::isfinite(q.bid) &&
           std::isfinite(q.ask) &&
           q.bid >= m.bid - EPS &&
           q.bid < m.ask &&
           q.ask > m.bid &&
           q.ask <= m.ask + EPS &&
           q.bid < q.ask;
}

long long allowed_buy_qty(long long inventory,
                          long long requested) {
    if (requested <= 0 ||
        inventory < -MAX_INVENTORY ||
        inventory > MAX_INVENTORY) {
        return 0;
    }

    return std::min(
        {requested, MAX_ORDER_SIZE,
         MAX_INVENTORY - inventory}
    );
}

long long allowed_sell_qty(long long inventory,
                           long long requested) {
    if (requested <= 0 ||
        inventory < -MAX_INVENTORY ||
        inventory > MAX_INVENTORY) {
        return 0;
    }

    return std::min(
        {requested, MAX_ORDER_SIZE,
         inventory + MAX_INVENTORY}
    );
}

int main() {
    std::ifstream file("data/sample_market_data.csv");

    if (!file) {
        std::cerr << "Cannot open data/sample_market_data.csv\n";
        return 1;
    }

    std::string line;
    bool header_checked = false;
    bool has_previous = false;
    long long previous_timestamp = 0;

    long long valid_count = 0;
    long long rejected_count = 0;

    std::cout << "\n=== MARKET-MAKER RISK CONTROLS ===\n\n";

    while (std::getline(file, line)) {
        if (line.find_first_not_of(" \t\r") ==
            std::string::npos) {
            continue;
        }

        if (!header_checked) {
            header_checked = true;
            if (line.find("timestamp") != std::string::npos) {
                continue;
            }
        }

        std::vector<std::string> fields;
        std::stringstream parser(line);
        std::string field;

        while (std::getline(parser, field, ',')) {
            fields.push_back(field);
        }

        if (fields.size() != 7) {
            ++rejected_count;
            std::cout << "BLOCKED: malformed CSV row\n";
            continue;
        }

        Market m{};

        try {
            m.timestamp = std::stoll(fields[0]);
            m.bid = std::stod(fields[1]);
            m.bid_qty = std::stoll(fields[2]);
            m.ask = std::stod(fields[3]);
            m.ask_qty = std::stoll(fields[4]);
            m.next_bid = std::stod(fields[5]);
            m.next_ask = std::stod(fields[6]);
        } catch (const std::exception&) {
            ++rejected_count;
            std::cout << "BLOCKED: invalid numeric data\n";
            continue;
        }

        // Validate the event sequence before considering a quote.
        const bool sequence_ok = valid_sequence(
            m.timestamp, has_previous, previous_timestamp
        );

        if (!has_previous || m.timestamp > previous_timestamp) {
            previous_timestamp = m.timestamp;
            has_previous = true;
        }

        if (!valid_market(m)) {
            ++rejected_count;
            std::cout << "BLOCKED: invalid market data at "
                      << m.timestamp << '\n';
            continue;
        }

        if (!sequence_ok) {
            ++rejected_count;
            std::cout << "BLOCKED: duplicate, out-of-order, "
                         "or missing snapshot at "
                      << m.timestamp << '\n';
            continue;
        }

        // Demonstration uses zero starting inventory for quote checks.
        const Quotes q = make_quotes(m, 0);

        if (!valid_quotes(m, q)) {
            ++rejected_count;
            std::cout << "BLOCKED: unsafe or crossed quotes at "
                      << m.timestamp << '\n';
            continue;
        }

        ++valid_count;

        std::cout << std::fixed << std::setprecision(2)
                  << "Snapshot " << m.timestamp
                  << " | PASS"
                  << " | Bid $" << q.bid
                  << " | Ask $" << q.ask
                  << " | Spread $" << q.ask - q.bid
                  << " | Max buy/sell order: "
                  << MAX_ORDER_SIZE << '\n';
    }

    // Run explicit edge-case checks.
    int passed = 0;
    int tests = 0;

    auto test = [&](const std::string& name, bool condition) {
        ++tests;

        if (condition) {
            ++passed;
            std::cout << "[PASS] " << name << '\n';
        } else {
            std::cout << "[FAIL] " << name << '\n';
        }
    };

    Market sample{1, 100.00, 80, 100.10, 20,
                  100.01, 100.11};

    Market crossed = sample;
    crossed.bid = 101.00;
    crossed.ask = 100.00;

    Market zero_depth = sample;
    zero_depth.bid_qty = 0;

    Market too_wide = sample;
    too_wide.ask = 101.00;

    Market invalid_next = sample;
    invalid_next.next_bid = 101.00;
    invalid_next.next_ask = 100.00;

    const Quotes sample_quotes = make_quotes(sample, 0);

    std::cout << "\n=== RISK EDGE-CASE TESTS ===\n";

    test("Reject crossed current market",
         !valid_market(crossed));

    test("Reject zero displayed bid depth",
         !valid_market(zero_depth));

    test("Reject excessively wide spread",
         !valid_market(too_wide));

    test("Reject crossed next market",
         !valid_market(invalid_next));

    test("Reject duplicate timestamp",
         !valid_sequence(5, true, 5));

    test("Reject missing snapshot sequence",
         !valid_sequence(7, true, 5));

    test("Accept quotes inside current market",
         valid_quotes(sample, sample_quotes));

    test("Block buys at maximum long inventory",
         allowed_buy_qty(30, 10) == 0);

    test("Block sells at maximum short inventory",
         allowed_sell_qty(-30, 10) == 0);

    test("Cap buy order at remaining inventory capacity",
         allowed_buy_qty(28, 10) == 2);

    test("Cap sell order at remaining inventory capacity",
         allowed_sell_qty(-28, 10) == 2);

    std::cout << "\n=== SUMMARY ===\n";
    std::cout << "Accepted snapshots: " << valid_count << '\n';
    std::cout << "Rejected snapshots: " << rejected_count << '\n';
    std::cout << "Risk tests passed: " << passed
              << " / " << tests << '\n';

    return passed == tests ? 0 : 1;
}
