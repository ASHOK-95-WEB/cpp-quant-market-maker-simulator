#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <string>

constexpr double TICK = 0.01;
constexpr double HALF_SPREAD = 0.03;
constexpr double INVENTORY_SKEW = 0.002;
constexpr double EPS = 1e-7;
constexpr long long MAX_INVENTORY = 30;
constexpr long long MAX_ORDER_SIZE = 10;

struct Market {
    double bid;
    long long bid_qty;
    double ask;
    long long ask_qty;
};

struct Quotes {
    double bid;
    double ask;
};

bool valid_market(const Market& m) {
    if (!std::isfinite(m.bid) ||
        !std::isfinite(m.ask) ||
        m.bid <= 0.0 ||
        m.ask <= m.bid ||
        m.bid_qty <= 0 ||
        m.ask_qty <= 0 ||
        m.ask - m.bid < TICK - EPS) {
        return false;
    }

    const double bid_ticks = m.bid / TICK;
    const double ask_ticks = m.ask / TICK;

    return std::abs(bid_ticks - std::round(bid_ticks)) < EPS &&
           std::abs(ask_ticks - std::round(ask_ticks)) < EPS;
}

double imbalance(const Market& m) {
    const double bid_qty = static_cast<double>(m.bid_qty);
    const double ask_qty = static_cast<double>(m.ask_qty);

    return (bid_qty - ask_qty) / (bid_qty + ask_qty);
}

Quotes make_quotes(const Market& m, long long inventory) {
    const double bq = static_cast<double>(m.bid_qty);
    const double aq = static_cast<double>(m.ask_qty);

    const double fair = (m.ask * bq + m.bid * aq) / (bq + aq);
    const double center =
        fair - INVENTORY_SKEW * static_cast<double>(inventory);

    const auto round_tick = [](double price) {
    return std::round(price / TICK) * TICK;
    };

    const double bid = std::clamp(
        round_tick(center - HALF_SPREAD),
        m.bid, m.ask - TICK
    );

    const double ask = std::clamp(
        round_tick(center + HALF_SPREAD),
        m.bid + TICK, m.ask
    );

    return {bid, ask};
}

long long allowed_buy_qty(long long inventory, long long requested) {
    if (requested <= 0 ||
        inventory < -MAX_INVENTORY ||
        inventory > MAX_INVENTORY) {
        return 0;
    }

    return std::min({
        requested,
        MAX_ORDER_SIZE,
        MAX_INVENTORY - inventory
    });
}

long long allowed_sell_qty(long long inventory, long long requested) {
    if (requested <= 0 ||
        inventory < -MAX_INVENTORY ||
        inventory > MAX_INVENTORY) {
        return 0;
    }

    return std::min({
        requested,
        MAX_ORDER_SIZE,
        inventory + MAX_INVENTORY
    });
}

bool check(const std::string& name, bool condition, int& passed) {
    if (condition) {
        ++passed;
        std::cout << "[PASS] " << name << '\n';
    } else {
        std::cout << "[FAIL] " << name << '\n';
    }

    return condition;
}

int main() {
    int passed = 0;
    int total = 0;

    auto test = [&](const std::string& name, bool condition) {
        ++total;
        check(name, condition, passed);
    };

    const Market balanced{100.00, 50, 100.10, 50};
    const Market bid_heavy{100.00, 80, 100.10, 20};
    const Market ask_heavy{100.00, 20, 100.10, 80};

    const Market crossed{100.10, 50, 100.00, 50};
    const Market no_depth{100.00, 0, 100.10, 50};

    const Quotes neutral_quotes = make_quotes(balanced, 0);
    const Quotes long_quotes = make_quotes(balanced, 5);
    const Quotes short_quotes = make_quotes(balanced, -5);

    std::cout << "\n=== STRATEGY UNIT TESTS ===\n\n";

    test("Balanced order book has zero imbalance",
         std::abs(imbalance(balanced)) < EPS);

    test("Bid-heavy book has positive imbalance",
         imbalance(bid_heavy) > 0.0);

    test("Ask-heavy book has negative imbalance",
         imbalance(ask_heavy) < 0.0);

    test("Imbalance stays in [-1, +1]",
         imbalance(bid_heavy) >= -1.0 &&
         imbalance(bid_heavy) <= 1.0);

    test("Valid market snapshot is accepted",
         valid_market(balanced));

    test("Crossed market is rejected",
         !valid_market(crossed));

    test("Zero-depth market is rejected",
         !valid_market(no_depth));

    test("Bid quote is below ask quote",
         neutral_quotes.bid < neutral_quotes.ask);

    test("Quotes remain within market bounds",
         neutral_quotes.bid >= balanced.bid &&
         neutral_quotes.ask <= balanced.ask);

    test("Long inventory shifts quotes downward",
         long_quotes.bid < neutral_quotes.bid &&
         long_quotes.ask < neutral_quotes.ask);

    test("Short inventory shifts quotes upward",
         short_quotes.bid > neutral_quotes.bid &&
         short_quotes.ask > neutral_quotes.ask);

    test("Buy blocked at maximum long inventory",
         allowed_buy_qty(30, 10) == 0);

    test("Sell blocked at maximum short inventory",
         allowed_sell_qty(-30, 10) == 0);

    test("Buy quantity respects remaining capacity",
         allowed_buy_qty(28, 10) == 2);

    test("Sell quantity respects remaining capacity",
         allowed_sell_qty(-28, 10) == 2);

    test("Buy order respects maximum order size",
         allowed_buy_qty(0, 100) == 10);

    test("Sell order respects maximum order size",
         allowed_sell_qty(0, 100) == 10);

    test("Negative requested buy quantity is rejected",
         allowed_buy_qty(0, -5) == 0);

    test("Negative requested sell quantity is rejected",
         allowed_sell_qty(0, -5) == 0);

    std::cout << "\nTests passed: "
              << passed << " / " << total << '\n';

    if (passed != total) {
        std::cerr << "Fix failed tests before benchmarking.\n";
        return 1;
    }

    // Microbenchmark: repeat calculations on varying inputs.
    constexpr long long ITERATIONS = 1'000'000;
    double checksum = 0.0;

    const auto start = std::chrono::steady_clock::now();

    for (long long i = 0; i < ITERATIONS; ++i) {
        const Market m{
            100.00,
            1 + (i % 100),
            100.10,
            1 + ((i * 7) % 100)
        };

        const long long inventory = (i % 61) - 30;

        const double signal = imbalance(m);
        const Quotes q = make_quotes(m, inventory);

        checksum += signal + q.bid + q.ask;
    }

    const auto finish = std::chrono::steady_clock::now();

    const double seconds =
        std::chrono::duration<double>(finish - start).count();

    std::cout << std::fixed << std::setprecision(3);
    std::cout << "\n=== C++ MICROBENCHMARK ===\n";
    std::cout << "Iterations: " << ITERATIONS << '\n';
    std::cout << "Elapsed time: " << seconds << " seconds\n";
    std::cout << "Calculations per second: "
              << ITERATIONS / seconds << '\n';
    std::cout << "Checksum: " << checksum << '\n';

    std::cout
        << "\nBenchmark measures these calculations only; "
           "it does not measure exchange-to-trade latency.\n";

    return 0;
}