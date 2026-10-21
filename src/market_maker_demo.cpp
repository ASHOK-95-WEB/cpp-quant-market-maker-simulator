#include <algorithm>
#include <cmath>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

struct MarketSnapshot {
    double bid;
    long long bid_qty;
    double ask;
    long long ask_qty;
};

struct Quotes {
    double bid;
    double ask;
    double fair_price;
};

constexpr double TICK = 0.01;
constexpr double HALF_SPREAD = 0.03;
constexpr double INVENTORY_SKEW = 0.002;

// Round a price to the nearest minimum tick.
double round_to_tick(double price) {
    return std::round(price / TICK) * TICK;
}

// Calculate quotes using fair value and inventory.
Quotes calculate_quotes(
    const MarketSnapshot& market,
    long long inventory
) {
    const double total_qty =
        static_cast<double>(market.bid_qty) +
        static_cast<double>(market.ask_qty);

    // Queue-weighted microprice estimate.
    const double fair =
        (market.ask * static_cast<double>(market.bid_qty) +
         market.bid * static_cast<double>(market.ask_qty))
        / total_qty;

    // Long inventory shifts quotes down.
    // Short inventory shifts quotes up.
    const double center =
        fair - INVENTORY_SKEW *
                   static_cast<double>(inventory);

    const double desired_bid =
        round_to_tick(center - HALF_SPREAD);

    const double desired_ask =
        round_to_tick(center + HALF_SPREAD);

    // Keep quotes inside the current market.
    const double bid_quote = std::clamp(
        desired_bid,
        market.bid,
        market.ask - TICK
    );

    const double ask_quote = std::clamp(
        desired_ask,
        market.bid + TICK,
        market.ask
    );

    return {bid_quote, ask_quote, fair};
}

int main() {
    std::ifstream file("data/sample_market_data.csv");

    if (!file) {
        std::cerr << "Cannot open sample market data.\n";
        return 1;
    }

    std::string line;
    bool header_skipped = false;
    bool first_snapshot = true;
    long long row_number = 0;

    std::cout << std::fixed << std::setprecision(2);

    std::cout << "\n=== INVENTORY-AWARE MARKET MAKER ===\n";
    std::cout << "Half-spread: $" << HALF_SPREAD << '\n';
    std::cout << "Inventory skew/share: $"
              << INVENTORY_SKEW << "\n\n";

    while (std::getline(file, line)) {
        if (line.find_first_not_of(" \t\r") ==
            std::string::npos) {
            continue;
        }

        if (!header_skipped) {
            header_skipped = true;

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
            std::cerr << "Skipping malformed CSV row.\n";
            continue;
        }

        MarketSnapshot market{};

        try {
            market.bid = std::stod(fields[1]);
            market.bid_qty = std::stoll(fields[2]);
            market.ask = std::stod(fields[3]);
            market.ask_qty = std::stoll(fields[4]);
        } catch (const std::exception&) {
            std::cerr << "Skipping invalid row.\n";
            continue;
        }

        if (market.bid > market.ask ||
            market.bid_qty < 0 ||
            market.ask_qty < 0 ||
            market.bid_qty + market.ask_qty <= 0 ||
            market.ask - market.bid < TICK) {
            std::cerr << "Skipping invalid market snapshot.\n";
            continue;
        }

        ++row_number;

        // Stage 1: quote generation with zero inventory.
        // Inventory will become dynamic once we implement fills.
        const Quotes q = calculate_quotes(market, 0);

        std::cout << "Snapshot " << row_number
                  << " | Fair: $" << q.fair_price
                  << " | Our bid: $" << q.bid
                  << " | Our ask: $" << q.ask
                  << " | Quote spread: $"
                  << q.ask - q.bid << '\n';

        // Show how inventory changes the first snapshot's quotes.
        if (first_snapshot) {
            first_snapshot = false;

            std::cout << "\nInventory sensitivity:\n";

            for (long long inventory : {-5LL, 0LL, 5LL}) {
                const Quotes adjusted =
                    calculate_quotes(market, inventory);

                std::cout << "Inventory: " << inventory
                          << " shares | Bid: $"
                          << adjusted.bid
                          << " | Ask: $"
                          << adjusted.ask << '\n';
            }

            std::cout << '\n';
        }
    }

    if (row_number == 0) {
        std::cerr << "No valid snapshots found.\n";
        return 1;
    }

    std::cout
        << "\nQuotes only: no orders have been filled, "
           "and no profit has been calculated.\n";

    return 0;
}