
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
    double next_bid;
    double next_ask;
};

struct Quotes {
    double bid;
    double ask;
    double fair;
};

struct Flow {
    char aggressor; // S = incoming seller; B = incoming buyer
    long long qty;
};

constexpr double TICK = 0.01;
constexpr double HALF_SPREAD = 0.03;
constexpr double INVENTORY_SKEW = 0.002;
constexpr double FEE_PER_SHARE = 0.005;
constexpr long long MAX_INVENTORY = 30;

double round_to_tick(double price) {
    return std::round(price / TICK) * TICK;
}

Quotes make_quotes(const Market& m, long long inventory) {
    const double bq = static_cast<double>(m.bid_qty);
    const double aq = static_cast<double>(m.ask_qty);
    const double total = bq + aq;

    const double fair = (m.ask * bq + m.bid * aq) / total;
    const double center =
        fair - INVENTORY_SKEW * static_cast<double>(inventory);

    const double bid = std::clamp(
        round_to_tick(center - HALF_SPREAD),
        m.bid, m.ask - TICK
    );

    const double ask = std::clamp(
        round_to_tick(center + HALF_SPREAD),
        m.bid + TICK, m.ask
    );

    return {bid, ask, fair};
}

int main() {
    std::ifstream file("data/sample_market_data.csv");

    if (!file) {
        std::cerr << "Cannot open data/sample_market_data.csv\n";
        return 1;
    }

    // Invented order flow, used only for education.
    const std::vector<Flow> flows = {
        {'S', 12}, {'B', 8},  {'B', 15}, {'S', 10},
        {'B', 9},  {'S', 14}, {'B', 11}, {'S', 7},
        {'B', 13}, {'S', 10}, {'B', 6},  {'S', 12}
    };

    long long inventory = 0;
    long long min_inventory = 0;
    long long max_inventory = 0;
    long long bought = 0;
    long long sold = 0;
    long long buy_fills = 0;
    long long sell_fills = 0;
    long long no_fills = 0;
    long long snapshots = 0;

    double cash = 0.0;
    double fees = 0.0;
    double peak_equity = 0.0;
    double max_drawdown = 0.0;
    double last_mid = 0.0;

    std::string line;
    bool header_checked = false;

    std::cout << std::fixed << std::setprecision(3);
    std::cout << "\n=== MARKET-MAKER INVENTORY LEDGER ===\n";
    std::cout << "Maximum absolute inventory: "
              << MAX_INVENTORY << " shares\n";
    std::cout << "Fee per filled share: $"
              << FEE_PER_SHARE << "\n\n";

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
            std::cerr << "Skipping malformed CSV row.\n";
            continue;
        }

        Market m{};

        try {
            m.bid = std::stod(fields[1]);
            m.bid_qty = std::stoll(fields[2]);
            m.ask = std::stod(fields[3]);
            m.ask_qty = std::stoll(fields[4]);
            m.next_bid = std::stod(fields[5]);
            m.next_ask = std::stod(fields[6]);
        } catch (const std::exception&) {
            std::cerr << "Skipping invalid CSV values.\n";
            continue;
        }

        if (m.bid > m.ask || m.next_bid > m.next_ask ||
            m.bid_qty < 0 || m.ask_qty < 0 ||
            m.bid_qty + m.ask_qty <= 0 ||
            m.ask - m.bid < TICK) {
            std::cerr << "Skipping invalid market snapshot.\n";
            continue;
        }

        if (snapshots >= static_cast<long long>(flows.size())) {
            break;
        }

        const Flow flow = flows[static_cast<std::size_t>(snapshots)];

        // Quotes now respond to the current inventory.
        const Quotes q = make_quotes(m, inventory);

        long long fill_qty = 0;
        char fill_side = '-';

        if (flow.aggressor == 'S' &&
            inventory < MAX_INVENTORY) {
            // Incoming seller hits our bid.
            const long long proposed =
                q.bid > m.bid + 1e-9
                    ? std::min(flow.qty, 10LL)
                    : std::max(1LL, flow.qty / 2);

            fill_qty = std::min(
                proposed, MAX_INVENTORY - inventory);

            if (fill_qty > 0) {
                inventory += fill_qty;
                cash -= q.bid * static_cast<double>(fill_qty);
                fees += FEE_PER_SHARE *
                        static_cast<double>(fill_qty);
                bought += fill_qty;
                ++buy_fills;
                fill_side = 'B';
            }
        } else if (flow.aggressor == 'B' &&
                   inventory > -MAX_INVENTORY) {
            // Incoming buyer lifts our ask.
            const long long proposed =
                q.ask < m.ask - 1e-9
                    ? std::min(flow.qty, 10LL)
                    : std::max(1LL, flow.qty / 2);

            fill_qty = std::min(
                proposed, inventory + MAX_INVENTORY);

            if (fill_qty > 0) {
                inventory -= fill_qty;
                cash += q.ask * static_cast<double>(fill_qty);
                fees += FEE_PER_SHARE *
                        static_cast<double>(fill_qty);
                sold += fill_qty;
                ++sell_fills;
                fill_side = 'S';
            }
        }

        if (fill_qty == 0) {
            ++no_fills;
        }

        ++snapshots;

        min_inventory = std::min(min_inventory, inventory);
        max_inventory = std::max(max_inventory, inventory);

        // Mark remaining inventory at the next observed midpoint.
        // It is a valuation estimate, not a guaranteed exit price.
        last_mid = (m.next_bid + m.next_ask) / 2.0;

        const double gross_equity =
            cash + static_cast<double>(inventory) * last_mid;

        const double net_equity = gross_equity - fees;

        peak_equity = std::max(peak_equity, net_equity);
        max_drawdown = std::max(
            max_drawdown, peak_equity - net_equity);

        std::cout << "Snapshot " << snapshots
                  << " | Quote: " << q.bid << " / " << q.ask
                  << " | Fill: ";

        if (fill_side == 'B') {
            std::cout << "BOUGHT ";
        } else if (fill_side == 'S') {
            std::cout << "SOLD ";
        } else {
            std::cout << "NONE ";
        }

        std::cout << fill_qty
                  << " | Inventory: " << inventory
                  << " | Cash: $" << cash
                  << " | Net equity: $" << net_equity << '\n';
    }

    const double gross_pnl =
        cash + static_cast<double>(inventory) * last_mid;
    const double net_pnl = gross_pnl - fees;

    std::cout << "\n=== LEDGER SUMMARY ===\n";
    std::cout << "Snapshots processed: " << snapshots << '\n';
    std::cout << "Buy-side fills: " << buy_fills
              << " | Shares bought: " << bought << '\n';
    std::cout << "Sell-side fills: " << sell_fills
              << " | Shares sold: " << sold << '\n';
    std::cout << "Unfilled events: " << no_fills << '\n';
    std::cout << "Final inventory: " << inventory << '\n';
    std::cout << "Minimum inventory: " << min_inventory << '\n';
    std::cout << "Maximum inventory: " << max_inventory << '\n';
    std::cout << "Cash balance before fees: $" << cash << '\n';
    std::cout << "Final mark price: $" << last_mid << '\n';
    std::cout << "Gross mark-to-market P&L: $" << gross_pnl << '\n';
    std::cout << "Estimated fees: $" << fees << '\n';
    std::cout << "Net mark-to-market P&L: $" << net_pnl << '\n';
    std::cout << "Maximum equity drawdown: $" << max_drawdown << '\n';

    std::cout
        << "\nSynthetic-data simulation only. Fill assumptions, "
           "queue priority, latency and slippage are simplified.\n";

    return 0;
}
