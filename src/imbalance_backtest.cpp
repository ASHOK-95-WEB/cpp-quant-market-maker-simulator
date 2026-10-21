
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

struct Snapshot {
    double bid_price;
    long long bid_qty;
    double ask_price;
    long long ask_qty;
    double next_mid;
};

int main() {
    std::ifstream file("data/sample_market_data.csv");

    if (!file) {
        std::cerr << "Error: Could not open market data CSV.\n";
        return 1;
    }

    constexpr double threshold = 0.20;
    constexpr double epsilon = 1e-12;

    long long rows = 0;
    long long buy_signals = 0;
    long long sell_signals = 0;
    long long neutral_rows = 0;
    long long flat_outcomes = 0;
    long long scored = 0;
    long long correct = 0;

    double signed_move_sum = 0.0;

    std::string line;
    std::getline(file, line); // Skip CSV header.

    while (std::getline(file, line)) {
        if (line.empty()) {
            continue;
        }

        std::vector<std::string> fields;
        std::stringstream parser(line);
        std::string field;

        while (std::getline(parser, field, ',')) {
            fields.push_back(field);
        }

        if (fields.size() != 6) {
            std::cerr << "Skipping malformed CSV row.\n";
            continue;
        }

        try {
            Snapshot s{
                std::stod(fields[1]),
                std::stoll(fields[2]),
                std::stod(fields[3]),
                std::stoll(fields[4]),
                std::stod(fields[5])
            };

            if (s.bid_qty < 0 || s.ask_qty < 0 ||
                s.bid_price > s.ask_price ||
                s.bid_qty + s.ask_qty == 0) {
                std::cerr << "Skipping invalid market snapshot.\n";
                continue;
            }

            ++rows;

            const double total_qty =
                static_cast<double>(s.bid_qty) +
                static_cast<double>(s.ask_qty);

            const double imbalance =
                (static_cast<double>(s.bid_qty) -
                 static_cast<double>(s.ask_qty)) / total_qty;

            const double mid =
                (s.bid_price + s.ask_price) / 2.0;

            int signal = 0;

            if (imbalance > threshold) {
                signal = 1;
                ++buy_signals;
            } else if (imbalance < -threshold) {
                signal = -1;
                ++sell_signals;
            } else {
                ++neutral_rows;
                continue;
            }

            // Evaluate the signal against the next observed mid.
            const double forward_move = s.next_mid - mid;

            if (std::abs(forward_move) < epsilon) {
                ++flat_outcomes;
                continue;
            }

            ++scored;

            // Positive = prediction was correct.
            // Negative = prediction was incorrect.
            const double signed_move = signal * forward_move;

            signed_move_sum += signed_move;

            if (signed_move > 0.0) {
                ++correct;
            }

        } catch (const std::exception&) {
            std::cerr << "Skipping invalid CSV values.\n";
        }
    }

    std::cout << std::fixed << std::setprecision(4);
    std::cout << "\n=== IMBALANCE BACKTEST REPORT ===\n\n";

    std::cout << "Valid snapshots: " << rows << '\n';
    std::cout << "Buy signals: " << buy_signals << '\n';
    std::cout << "Sell signals: " << sell_signals << '\n';
    std::cout << "Neutral snapshots: " << neutral_rows << '\n';
    std::cout << "Flat signal outcomes: " << flat_outcomes << '\n';
    std::cout << "Scored signals: " << scored << '\n';

    if (scored > 0) {
        const double hit_rate =
            100.0 * static_cast<double>(correct) /
            static_cast<double>(scored);

        const double average_signed_move =
            signed_move_sum / static_cast<double>(scored);

        std::cout << "Correct directional predictions: "
                  << correct << '\n';

        std::cout << "Directional hit rate: "
                  << hit_rate << "%\n";

        std::cout << "Average signed forward move: $"
                  << average_signed_move
                  << " per share\n";
    } else {
        std::cout << "Not enough non-flat outcomes to score.\n";
    }

    std::cout << "\nResearch demo only. No trading costs included.\n";

    return 0;
}
