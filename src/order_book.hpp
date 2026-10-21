#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace lob {

using OrderId = std::uint64_t;
using Price = std::int64_t;  // integer ticks, never floating point
using Qty = std::int64_t;

enum class Side { Buy, Sell };

struct Order {
    OrderId id;
    Side side;
    Price price;
    Qty qty;
};

struct Trade {
    OrderId taker_id;  // the incoming order
    OrderId maker_id;  // the resting order
    Price price;       // always the resting order's price
    Qty qty;
};

class OrderBook {
public:
    // Adds a limit order. Matches against the opposite side first (price-time
    // priority); any unfilled remainder rests in the book.
    // Returns the trades produced. Invalid input (qty <= 0, duplicate id)
    // is rejected and returns no trades.
    std::vector<Trade> add_limit(OrderId id, Side side, Price price, Qty qty);

    // Removes a resting order. Returns false if the id is not in the book
    // (never existed, already filled, or already cancelled).
    bool cancel(OrderId id);

    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;
    Qty depth_at(Side side, Price price) const;  // total resting qty at a price
    std::size_t order_count() const { return index_.size(); }

private:
    using Level = std::list<Order>;  // FIFO queue: front = oldest = first to fill
    using Bids = std::map<Price, Level, std::greater<Price>>;  // best = highest
    using Asks = std::map<Price, Level>;                       // best = lowest

    struct Locator {
        Side side;
        Price price;
        Level::iterator it;
    };

    template <class BookSide, class Crosses>
    void match(Order& incoming, BookSide& book, Crosses crosses,
               std::vector<Trade>& trades);

    template <class BookSide>
    void rest(BookSide& book, const Order& o);

    Bids bids_;
    Asks asks_;
    std::unordered_map<OrderId, Locator> index_;  // O(1) cancel lookup
};

}  // namespace lob
