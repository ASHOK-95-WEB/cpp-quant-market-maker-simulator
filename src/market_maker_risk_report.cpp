
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
};

struct Flow {
    char aggressor;
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

    const double fair =
        (m.ask * bq + m.bid * aq) / total;

    const double center =
        fair - INVENTORY_SKEW *
                   static_cast<double>(inventory);

    const double bid_quote = std::clamp(
        round_to_tick(center - HALF_SPREAD),
        m.bid, m.ask - TICK
    );

    const double ask_quote = std::clamp(
        round_to_tick(center + HALF_SPREAD),
        m.bid + TICK, m.ask
    );

    return {bid_quote, ask_quote};
}

int main() {
    std::ifstream file("data/sample_market_data.csv");

    if (!file) {
        std::cerr << "Cannot open market data CSV.\n";
        return 1;
    }

    const std::vector<Flow> flows = {
        {'S', 12}, {'B', 8},  {'B', 15}, {'S', 10},
        {'B', 9},  {'S', 14}, {'B', 11}, {'S', 7},
        {'B', 13}, {'S', 10}, {'B', 6},  {'S', 12}
    };

    long long snapshots = 0;
    long long fill_events = 0;
    long long buy_events = 0;
    long long sell_events = 0;
    long long no_fills = 0;
    long long shares_bought = 0;
    long long shares_sold = 0;

    long long inventory = 0;
    long long min_inventory = 0;
    long long max_inventory = 0;
    long long peak_abs_inventory = 0;
    double sum_abs_inventory = 0.0;

    double cash = 0.0;
    double fees = 0.0;
    double buy_turnover = 0.0;
    double sell_turnover = 0.0;
    double last_mid = 0.0;

    double peak_equity = 0.0;
    double max_drawdown = 0.0;

    std::string line;
    bool header_checked = false;

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

        if (!std::isfinite(m.bid) ||
            !std::isfinite(m.ask) ||
            !std::isfinite(m.next_bid) ||
            !std::isfinite(m.next_ask) ||
            m.bid > m.ask ||
            m.next_bid > m.next_ask ||
            m.bid_qty < 0 || m.ask_qty < 0 ||
            m.bid_qty + m.ask_qty <= 0 ||
            m.ask - m.bid < TICK) {
            std::cerr << "Skipping invalid snapshot.\n";
            continue;
        }

        if (snapshots >= static_cast<long long>(flows.size())) {
            break;
        }

        const Flow flow = flows[static_cast<std::size_t>(snapshots)];
        const Quotes q = make_quotes(m, inventory);

        long long fill_qty = 0;
        char fill_side = '-';
        double fill_price = 0.0;

        if (flow.aggressor == 'S' &&
            inventory < MAX_INVENTORY) {

            const long long proposed =
                q.bid > m.bid + 1e-9
                    ? std::min(flow.qty, 10LL)
                    : std::max(1LL, flow.qty / 2);

            fill_qty = std::min(
                proposed, MAX_INVENTORY - inventory
            );

            if (fill_qty > 0) {
                fill_price = q.bid;
                fill_side = 'B';

                inventory += fill_qty;
                cash -= fill_price *
                        static_cast<double>(fill_qty);

                buy_turnover += fill_price *
                                static_cast<double>(fill_qty);

                shares_bought += fill_qty;
                ++buy_events;
            }

        } else if (flow.aggressor == 'B' &&
                   inventory > -MAX_INVENTORY) {

            const long long proposed =
                q.ask < m.ask - 1e-9
                    ? std::min(flow.qty, 10LL)
                    : std::max(1LL, flow.qty / 2);

            fill_qty = std::min(
                proposed, inventory + MAX_INVENTORY
            );

            if (fill_qty > 0) {
                fill_price = q.ask;
                fill_side = 'S';

                inventory -= fill_qty;
                cash += fill_price *
                        static_cast<double>(fill_qty);

                sell_turnover += fill_price *
                                 static_cast<double>(fill_qty);

                shares_sold += fill_qty;
                ++sell_events;
            }
        }

        if (fill_qty > 0) {
            ++fill_events;
            fees += FEE_PER_SHARE *
                    static_cast<double>(fill_qty);
        } else {
            ++no_fills;
        }

        ++snapshots;

        min_inventory = std::min(min_inventory, inventory);
        max_inventory = std::max(max_inventory, inventory);

        const long long abs_inventory =
            inventory < 0 ? -inventory : inventory;

        peak_abs_inventory =
            std::max(peak_abs_inventory, abs_inventory);

        sum_abs_inventory +=
            static_cast<double>(abs_inventory);

        last_mid = (m.next_bid + m.next_ask) / 2.0;

        const double equity =
            cash + static_cast<double>(inventory) * last_mid
                 - fees;

        peak_equity = std::max(peak_equity, equity);

        max_drawdown = std::max(
            max_drawdown, peak_equity - equity
        );

        std::cout << std::fixed << std::setprecision(2)
                  << "Snapshot " << snapshots
                  << " | Fill: ";

        if (fill_side == 'B') {
            std::cout << "BUY ";
        } else if (fill_side == 'S') {
            std::cout << "SELL ";
        } else {
            std::cout << "NONE ";
        }

        std::cout << fill_qty
                  << " | Inventory: " << inventory
                  << " | Marked equity: $" << equity << '\n';
    }

    if (snapshots == 0) {
        std::cerr << "No valid market snapshots processed.\n";
        return 1;
    }

    const double gross_pnl =
        cash + static_cast<double>(inventory) * last_mid;

    const double net_pnl = gross_pnl - fees;
    const double turnover = buy_turnover + sell_turnover;

    const double fill_rate =
        100.0 * static_cast<double>(fill_events) /
        static_cast<double>(snapshots);

    const double avg_abs_inventory =
        sum_abs_inventory / static_cast<double>(snapshots);

    const double inventory_utilization =
        100.0 * static_cast<double>(peak_abs_inventory) /
        static_cast<double>(MAX_INVENTORY);

    const double turnover_bps =
        turnover > 0.0 ? net_pnl / turnover * 10000.0 : 0.0;

    std::cout << std::fixed << std::setprecision(4);

    std::cout << "\n=== MARKET-MAKER RISK REPORT ===\n\n";

    std::cout << "Snapshots processed: " << snapshots << '\n';
    std::cout << "Filled events: " << fill_events << '\n';
    std::cout << "Unfilled events: " << no_fills << '\n';
    std::cout << "Fill-event rate: " << fill_rate << "%\n";

    std::cout << "\n--- Trading activity ---\n";
    std::cout << "Buy fills: " << buy_events
              << " | Shares bought: " << shares_bought << '\n';
    std::cout << "Sell fills: " << sell_events
              << " | Shares sold: " << shares_sold << '\n';
    std::cout << "Buy turnover: $" << buy_turnover << '\n';
    std::cout << "Sell turnover: $" << sell_turnover << '\n';
    std::cout << "Total turnover: $" << turnover << '\n';

    std::cout << "\n--- Inventory risk ---\n";
    std::cout << "Final inventory: " << inventory << '\n';
    std::cout << "Minimum inventory: " << min_inventory << '\n';
    std::cout << "Maximum inventory: " << max_inventory << '\n';
    std::cout << "Peak absolute inventory: "
              << peak_abs_inventory << '\n';
    std::cout << "Average absolute inventory: "
              << avg_abs_inventory << '\n';
    std::cout << "Inventory-limit utilization: "
              << inventory_utilization << "%\n";

    std::cout << "\n--- P&L and risk ---\n";
    std::cout << "Cash before fees: $" << cash << '\n';
    std::cout << "Final mark price: $" << last_mid << '\n';
    std::cout << "Gross mark-to-market P&L: $" << gross_pnl << '\n';
    std::cout << "Fees: $" << fees << '\n';
    std::cout << "Net mark-to-market P&L: $" << net_pnl << '\n';
    std::cout << "Net P&L / turnover: "
              << turnover_bps << " basis points\n";
    std::cout << "Average net P&L / snapshot: $"
              << net_pnl / static_cast<double>(snapshots) << '\n';
    std::cout << "Maximum drawdown: $" << max_drawdown << '\n';

    std::cout
        << "\nEducational simulation on synthetic data. "
           "Fill assumptions, market impact and latency "
           "are simplified.\n";

    return 0;
}
