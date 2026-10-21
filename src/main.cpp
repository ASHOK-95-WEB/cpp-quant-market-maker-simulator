#include <iostream>

#include "order_book.hpp"

using namespace lob;

static void print_trades(const std::vector<Trade>& trades) {
    for (const Trade& t : trades)
        std::cout << "  TRADE taker=" << t.taker_id << " maker=" << t.maker_id
                  << " price=" << t.price << " qty=" << t.qty << "\n";
}

static void print_top(const OrderBook& b) {
    std::cout << "  best bid: ";
    if (auto p = b.best_bid()) std::cout << *p; else std::cout << "-";
    std::cout << " | best ask: ";
    if (auto p = b.best_ask()) std::cout << *p; else std::cout << "-";
    std::cout << "\n";
}

int main() {
    OrderBook book;

    std::cout << "Add resting sells: 5@101 (id1), 5@102 (id2)\n";
    book.add_limit(1, Side::Sell, 101, 5);
    book.add_limit(2, Side::Sell, 102, 5);
    print_top(book);

    std::cout << "Add resting buy: 10@99 (id3)\n";
    book.add_limit(3, Side::Buy, 99, 10);
    print_top(book);

    std::cout << "Aggressive buy 8@102 (id4) sweeps two levels\n";
    print_trades(book.add_limit(4, Side::Buy, 102, 8));
    print_top(book);

    std::cout << "Cancel id3: " << (book.cancel(3) ? "ok" : "not found") << "\n";
    print_top(book);
    return 0;
}
