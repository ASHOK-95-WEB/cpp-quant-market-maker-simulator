
#include <iomanip>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <exception>

struct Snapshot {
    double bid;
    long long bid_qty;
    double ask;
    long long ask_qty;
    double next_bid;
    double next_ask;
};

int main() {
    std::ifstream file("data/sample_market_data.csv");

    if (!file) {
        std::cerr << "Cannot open CSV file.\n";
        return 1;
    }

    constexpr double threshold = 0.20;
    constexpr double fees_per_trade = 0.01;

    long long snapshots = 0;
    long long buy_signals = 0;
    long long sell_signals = 0;
    long long neutral = 0;
    long long trades = 0;
    long long profitable = 0;

    double gross_pnl = 0.0;
    double net_pnl = 0.0;
    double cumulative_pnl = 0.0;
    double peak_pnl = 0.0;
    double max_drawdown = 0.0;

    std::string line;
    bool header_skipped = false;

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

        Snapshot s{};

        try {
            s.bid = std::stod(fields[1]);
            s.bid_qty = std::stoll(fields[2]);
            s.ask = std::stod(fields[3]);
            s.ask_qty = std::stoll(fields[4]);
            s.next_bid = std::stod(fields[5]);
            s.next_ask = std::stod(fields[6]);
        } catch (const std::exception&) {
            std::cerr << "Skipping invalid CSV values.\n";
            continue;
        }

        if (s.bid_qty < 0 || s.ask_qty < 0 ||
            s.bid > s.ask ||
            s.next_bid > s.next_ask ||
            s.bid_qty + s.ask_qty <= 0) {
            std::cerr << "Skipping invalid market snapshot.\n";
            continue;
        }

        ++snapshots;

        const double total_qty =
            static_cast<double>(s.bid_qty) +
            static_cast<double>(s.ask_qty);

        const double imbalance =
            (static_cast<double>(s.bid_qty) -
             static_cast<double>(s.ask_qty)) / total_qty;

        int signal = 0;

        if (imbalance > threshold) {
            signal = 1;
            ++buy_signals;
        } else if (imbalance < -threshold) {
            signal = -1;
            ++sell_signals;
        } else {
            ++neutral;
            continue;
        }

        // Buy at the ask and exit at the next bid.
        // Sell/short at the bid and exit at the next ask.
        const double pnl =
            signal == 1
                ? s.next_bid - s.ask
                : s.bid - s.next_ask;

        const double net = pnl - fees_per_trade;

        ++trades;
        gross_pnl += pnl;
        net_pnl += net;

        if (net > 1e-9) {
            ++profitable;
        }

        cumulative_pnl += net;
        peak_pnl = std::max(peak_pnl, cumulative_pnl);

        max_drawdown = std::max(
            max_drawdown, peak_pnl - cumulative_pnl);
    }

    std::cout << std::fixed << std::setprecision(4);
    std::cout << "\n=== EXECUTABLE-PRICE P&L ESTIMATE ===\n";
    std::cout << "Snapshots: " << snapshots << '\n';
    std::cout << "Buy signals: " << buy_signals << '\n';
    std::cout << "Sell signals: " << sell_signals << '\n';
    std::cout << "Neutral snapshots: " << neutral << '\n';
    std::cout << "Signals evaluated: " << trades << '\n';
    std::cout << "Gross P&L: $" << gross_pnl << '\n';
    std::cout << "Estimated fees: $"
              << trades * fees_per_trade << '\n';
    std::cout << "Net P&L: $" << net_pnl << '\n';
    std::cout << "Average net per trade: $"
              << (trades ? net_pnl / trades : 0.0) << '\n';
    std::cout << "Profitable trades after fees: "
              << profitable << " / " << trades << '\n';
    std::cout << "Maximum drawdown: $"
              << max_drawdown << '\n';

    std::cout
        << "\nSynthetic-data estimate only; fills, latency, "
           "slippage, and market impact remain simplified.\n";

    return 0;
}
