#include "order_book.hpp"

#include <algorithm>

namespace lob {

template <class BookSide, class Crosses>
void OrderBook::match(Order& in, BookSide& book, Crosses crosses,
                      std::vector<Trade>& trades) {
    while (in.qty > 0 && !book.empty()) {
        auto level_it = book.begin();  // best price on the opposite side
        if (!crosses(level_it->first)) break;

        Level& level = level_it->second;
        while (in.qty > 0 && !level.empty()) {
            Order& maker = level.front();  // oldest order at this price
            const Qty fill = std::min(in.qty, maker.qty);
            trades.push_back({in.id, maker.id, maker.price, fill});
            in.qty -= fill;
            maker.qty -= fill;
            if (maker.qty == 0) {
                index_.erase(maker.id);
                level.pop_front();
            }
        }
        if (level.empty()) book.erase(level_it);
    }
}

template <class BookSide>
void OrderBook::rest(BookSide& book, const Order& o) {
    Level& level = book[o.price];
    level.push_back(o);
    index_.emplace(o.id, Locator{o.side, o.price, std::prev(level.end())});
}

std::vector<Trade> OrderBook::add_limit(OrderId id, Side side, Price price,
                                        Qty qty) {
    std::vector<Trade> trades;
    if (qty <= 0 || index_.count(id) != 0) return trades;

    Order in{id, side, price, qty};
    if (side == Side::Buy) {
        match(in, asks_, [&](Price ask) { return ask <= in.price; }, trades);
        if (in.qty > 0) rest(bids_, in);
    } else {
        match(in, bids_, [&](Price bid) { return bid >= in.price; }, trades);
        if (in.qty > 0) rest(asks_, in);
    }
    return trades;
}

bool OrderBook::cancel(OrderId id) {
    auto it = index_.find(id);
    if (it == index_.end()) return false;

    const Locator loc = it->second;
    auto erase_from = [&](auto& book) {
        auto level_it = book.find(loc.price);
        level_it->second.erase(loc.it);
        if (level_it->second.empty()) book.erase(level_it);
    };
    if (loc.side == Side::Buy) erase_from(bids_);
    else erase_from(asks_);

    index_.erase(it);
    return true;
}

std::optional<Price> OrderBook::best_bid() const {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

Qty OrderBook::depth_at(Side side, Price price) const {
    auto sum = [&](const auto& book) -> Qty {
        auto it = book.find(price);
        if (it == book.end()) return 0;
        Qty total = 0;
        for (const Order& o : it->second) total += o.qty;
        return total;
    };
    return side == Side::Buy ? sum(bids_) : sum(asks_);
}

}  // namespace lob
