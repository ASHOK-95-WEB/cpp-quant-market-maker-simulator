
#include <iomanip>
#include <iostream>

#include "order_book.hpp"

using namespace lob;

// For this demonstration, 1 tick = $0.01.
double to_dollars(Price ticks) {
    return static_cast<double>(ticks) * 0.01;
}

int main() {
    OrderBook book;

    // Add resting buy orders.
    book.add_limit(1, Side::Buy, 10000, 80);
    book.add_limit(2, Side::Buy, 9990, 25);

    // Add resting sell orders.
    book.add_limit(3, Side::Sell, 10010, 20);
    book.add_limit(4, Side::Sell, 10020, 35);

    const auto bid = book.best_bid();
    const auto ask = book.best_ask();

    if (!bid || !ask) {
        std::cerr << "Insufficient market data.\n";
        return 1;
    }

    const Qty bid_qty = book.depth_at(Side::Buy, *bid);
    const Qty ask_qty = book.depth_at(Side::Sell, *ask);
    const Qty total_qty = bid_qty + ask_qty;

    if (total_qty <= 0) {
        std::cerr << "Invalid market depth.\n";
        return 1;
    }

    // Order-book imbalance ranges from -1 to +1.
    const double imbalance =
        static_cast<double>(bid_qty - ask_qty) / total_qty;

    // Mid-price: the average of the best bid and ask.
    const double mid_price =
        (to_dollars(*bid) + to_dollars(*ask)) / 2.0;

    // Microprice weights the opposing price by queue depth.
    const double microprice =
        (to_dollars(*ask) * bid_qty +
         to_dollars(*bid) * ask_qty) / total_qty;

    const char* signal = "NEUTRAL";

    // These thresholds are illustrative, not optimized.
    if (imbalance > 0.20) {
        signal = "BUY PRESSURE";
    } else if (imbalance < -0.20) {
        signal = "SELL PRESSURE";
    }

    std::cout << std::fixed << std::setprecision(2);

    std::cout << "\n=== QUANT MARKET SIGNAL DEMO ===\n\n";

    std::cout << "Best bid: $" << to_dollars(*bid)
              << " | Quantity: " << bid_qty << '\n';

    std::cout << "Best ask: $" << to_dollars(*ask)
              << " | Quantity: " << ask_qty << '\n';

    std::cout << "Mid-price: $" << mid_price << '\n';
    std::cout << "Microprice: $" << microprice << '\n';

    std::cout << std::setprecision(3);
    std::cout << "Order-book imbalance: " << imbalance << '\n';

    std::cout << "\nSignal: " << signal << '\n';

    std::cout << "\nEducational signal only; profitability "
                 "has not been tested.\n";

    return 0;
}
