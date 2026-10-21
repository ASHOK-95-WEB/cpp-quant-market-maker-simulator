
#include <algorithm>
#include <cmath>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

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

struct SimulatedFlow {
    char aggressor;  // B = incoming buyer, S = incoming seller
    long long qty;
};

constexpr double TICK = 0.01;
constexpr double HALF_SPREAD = 0.03;
constexpr double INVENTORY_SKEW = 0.002;

double round_to_tick(double price) {
    return std::round(price / TICK) * TICK;
}

Quotes make_quotes(const Market& m, long long inventory) {
    const double bid_qty = static_cast<double>(m.bid_qty);
    const double ask_qty = static_cast<double>(m.ask_qty);
    const double total = bid_qty + ask_qty;

    const double fair =
        (m.ask * bid_qty + m.bid * ask_qty) / total;

    const double center =
        fair - INVENTORY_SKEW *
                   static_cast<double>(inventory);

    const double our_bid = std::clamp(
        round_to_tick(center - HALF_SPREAD),
        m.bid,
        m.ask - TICK
    );

    const double our_ask = std::clamp(
        round_to_tick(center + HALF_SPREAD),
        m.bid + TICK,
        m.ask
    );

    return {our_bid, our_ask};
}

int main() {
    std::ifstream file("data/sample_market_data.csv");

    if (!file) {
        std::cerr << "Cannot open sample market data.\n";
        return 1;
    }

    // Invented order flow for demonstration purposes.
    // It is not actual exchange trade data.
    const std::vector<SimulatedFlow> flows = {
        {'S', 12}, {'B', 8},  {'B', 15}, {'S', 10},
        {'B', 9},  {'S', 14}, {'B', 11}, {'S', 7},
        {'B', 13}, {'S', 10}, {'B', 6},  {'S', 12}
    };

    long long snapshots = 0;
    long long buy_fills = 0;
    long long sell_fills = 0;
    long long buy_fill_qty = 0;
    long long sell_fill_qty = 0;
    long long no_fills = 0;

    std::string line;
    bool header_skipped = false;

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "\n=== MARKET-MAKER FILL SIMULATOR ===\n";
    std::cout << "Assumed maximum fill at a better price: 10 shares\n";
    std::cout << "Assumed fill at equal best price: half the flow\n\n";

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

        Market m{};

        try {
            m.bid = std::stod(fields[1]);
            m.bid_qty = std::stoll(fields[2]);
            m.ask = std::stod(fields[3]);
            m.ask_qty = std::stoll(fields[4]);
        } catch (const std::exception&) {
            std::cerr << "Skipping invalid CSV row.\n";
            continue;
        }

        if (m.bid > m.ask || m.bid_qty < 0 ||
            m.ask_qty < 0 || m.bid_qty + m.ask_qty <= 0 ||
            m.ask - m.bid < TICK) {
            std::cerr << "Skipping invalid market snapshot.\n";
            continue;
        }

        if (snapshots >= static_cast<long long>(flows.size())) {
            break;
        }

        const SimulatedFlow flow =
            flows[static_cast<std::size_t>(snapshots)];

        // Inventory is held at zero in this step.
        // Dynamic inventory management comes in Step 14.
        const Quotes q = make_quotes(m, 0);

        const bool bid_is_competitive =
            q.bid >= m.bid - 1e-9 &&
            q.bid < m.ask;

        const bool ask_is_competitive =
            q.ask <= m.ask + 1e-9 &&
            q.ask > m.bid;

        std::string result = "NO FILL";
        long long filled_qty = 0;

        if (flow.aggressor == 'S' && bid_is_competitive) {
            // A better bid is modeled as receiving priority.
            if (q.bid > m.bid + 1e-9) {
                filled_qty = std::min(flow.qty, 10LL);
            } else {
                filled_qty = std::max(1LL, flow.qty / 2);
            }

            result = "OUR BID FILLED";
            ++buy_fills;
            buy_fill_qty += filled_qty;

        } else if (flow.aggressor == 'B' &&
                   ask_is_competitive) {
            if (q.ask < m.ask - 1e-9) {
                filled_qty = std::min(flow.qty, 10LL);
            } else {
                filled_qty = std::max(1LL, flow.qty / 2);
            }

            result = "OUR ASK FILLED";
            ++sell_fills;
            sell_fill_qty += filled_qty;

        } else {
            ++no_fills;
        }

        ++snapshots;

        std::cout
            << "Snapshot " << snapshots
            << " | Incoming "
            << (flow.aggressor == 'B' ? "BUY " : "SELL")
            << flow.qty
            << " | Our bid $" << q.bid
            << " | Our ask $" << q.ask
            << " | " << result;

        if (filled_qty > 0) {
            std::cout << " (" << filled_qty << " shares)";
        }

        std::cout << '\n';
    }

    std::cout << "\n=== FILL SUMMARY ===\n";
    std::cout << "Snapshots processed: " << snapshots << '\n';
    std::cout << "Buy-side fills: " << buy_fills
              << " | Shares bought: " << buy_fill_qty << '\n';
    std::cout << "Sell-side fills: " << sell_fills
              << " | Shares sold: " << sell_fill_qty << '\n';
    std::cout << "No fills: " << no_fills << '\n';

    std::cout
        << "\nEducational simulation only. Fill priority and "
           "quantities are simplified assumptions.\n";

    return 0;
}
