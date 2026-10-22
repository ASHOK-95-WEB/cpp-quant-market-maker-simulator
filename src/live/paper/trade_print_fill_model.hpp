#pragma once

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace paper {

// Side of our hypothetical resting limit order.
enum class RestingSide {
    Buy,
    Sell
};

// Side of the aggressive taker that generated an exchange trade print.
// For Binance aggTrade data: m=true means the buyer was maker, so the
// aggressor was a seller; m=false means the aggressor was a buyer.
enum class AggressorSide {
    Buy,
    Sell
};

struct TradePrint {
    double price{0.0};
    double quantity{0.0};
    AggressorSide aggressor{AggressorSide::Buy};
};

struct RestingOrder {
    RestingSide side{RestingSide::Buy};
    double price{0.0};
    double original_quantity{0.0};
    double remaining_quantity{0.0};
    double queue_ahead_quantity{0.0};
    double filled_quantity{0.0};
    bool active{false};
};

// Describes the incremental fill attributed to one trade print.
struct FillDelta {
    double price{0.0};
    double quantity{0.0};

    explicit operator bool() const noexcept {
        return quantity > 0.0;
    }
};

inline RestingOrder make_resting_order(RestingSide side,
                                       double price,
                                       double quantity,
                                       double estimated_queue_ahead_quantity) {
    if (!std::isfinite(price) || price <= 0.0 ||
        !std::isfinite(quantity) || quantity <= 0.0 ||
        !std::isfinite(estimated_queue_ahead_quantity) ||
        estimated_queue_ahead_quantity < 0.0) {
        throw std::invalid_argument(
            "Resting order price/quantity must be positive and queue ahead non-negative");
    }

    return RestingOrder{side, price, quantity, quantity,
                        estimated_queue_ahead_quantity, 0.0, true};
}

// Apply one public trade print to one hypothetical resting order.
// We only attribute volume at the order's price and in the matching direction.
// First, trade volume consumes estimated quantity already ahead in the queue;
// only the remaining volume can fill our simulated order.
//
// This is an educational approximation: displayed queue ahead is an estimate,
// cancellations/hidden liquidity are unknown, and exact matching-engine queue
// priority cannot be inferred from public aggregate trade prints alone.
inline FillDelta apply_trade_print(RestingOrder& order,
                                   const TradePrint& trade,
                                   double price_tolerance = 1e-6) {
    constexpr double kQuantityEpsilon = 1e-12;

    if (!std::isfinite(price_tolerance) || price_tolerance < 0.0) {
        throw std::invalid_argument("Price tolerance must be finite and non-negative");
    }

    if (!order.active || order.remaining_quantity <= kQuantityEpsilon) {
        return {};
    }

    if (!std::isfinite(trade.price) || trade.price <= 0.0 ||
        !std::isfinite(trade.quantity) || trade.quantity <= 0.0) {
        return {};
    }

    const bool direction_matches =
        (order.side == RestingSide::Buy && trade.aggressor == AggressorSide::Sell) ||
        (order.side == RestingSide::Sell && trade.aggressor == AggressorSide::Buy);
    if (!direction_matches || std::abs(trade.price - order.price) > price_tolerance) {
        return {};
    }

    const double queue_consumed =
        std::min(order.queue_ahead_quantity, trade.quantity);
    order.queue_ahead_quantity =
        std::max(0.0, order.queue_ahead_quantity - queue_consumed);

    const double volume_after_queue = trade.quantity - queue_consumed;
    if (volume_after_queue <= kQuantityEpsilon) {
        return {};
    }

    const double fill_quantity = std::min(order.remaining_quantity, volume_after_queue);
    if (fill_quantity <= kQuantityEpsilon) {
        return {};
    }

    order.remaining_quantity =
        std::max(0.0, order.remaining_quantity - fill_quantity);
    order.filled_quantity += fill_quantity;
    if (order.remaining_quantity <= kQuantityEpsilon) {
        order.remaining_quantity = 0.0;
        order.active = false;
    }

    // A passive limit fill is booked at our order's limit price, not a better
    // or worse price inferred from an unrelated level.
    return FillDelta{order.price, fill_quantity};
}

} // namespace paper