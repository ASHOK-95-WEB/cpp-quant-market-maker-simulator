#include "trade_print_fill_model.hpp"

#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool near(double actual, double expected, double tolerance = 1e-9) {
    return std::abs(actual - expected) <= tolerance;
}

void test_bid_requires_sell_aggressor_at_order_price() {
    auto order = paper::make_resting_order(paper::RestingSide::Buy, 100.0, 1.0, 0.0);

    auto fill = paper::apply_trade_print(
        order, paper::TradePrint{100.0, 1.0, paper::AggressorSide::Buy});
    require(!fill && near(order.remaining_quantity, 1.0),
            "buy aggressor must not fill our resting bid");

    fill = paper::apply_trade_print(
        order, paper::TradePrint{100.01, 1.0, paper::AggressorSide::Sell});
    require(!fill && near(order.remaining_quantity, 1.0),
            "trade at a different price must not fill our resting bid");

    fill = paper::apply_trade_print(
        order, paper::TradePrint{100.0, 1.0, paper::AggressorSide::Sell});
    require(static_cast<bool>(fill) && near(fill.price, 100.0) &&
                near(fill.quantity, 1.0) && !order.active,
            "sell aggressor at the bid should complete the resting buy order");
}

void test_ask_requires_buy_aggressor() {
    auto order = paper::make_resting_order(paper::RestingSide::Sell, 101.0, 0.5, 0.0);
    auto fill = paper::apply_trade_print(
        order, paper::TradePrint{101.0, 0.5, paper::AggressorSide::Sell});
    require(!fill && near(order.remaining_quantity, 0.5),
            "sell aggressor must not fill our resting ask");

    fill = paper::apply_trade_print(
        order, paper::TradePrint{101.0, 0.5, paper::AggressorSide::Buy});
    require(static_cast<bool>(fill) && near(fill.quantity, 0.5) && !order.active,
            "buy aggressor at the ask should complete the resting sell order");
}

void test_queue_ahead_is_consumed_before_our_fill() {
    auto order = paper::make_resting_order(paper::RestingSide::Buy, 100.0, 1.0, 1.5);

    auto fill = paper::apply_trade_print(
        order, paper::TradePrint{100.0, 1.0, paper::AggressorSide::Sell});
    require(!fill && near(order.queue_ahead_quantity, 0.5) &&
                near(order.remaining_quantity, 1.0),
            "first trade should reduce queue ahead without filling our order");

    fill = paper::apply_trade_print(
        order, paper::TradePrint{100.0, 1.0, paper::AggressorSide::Sell});
    require(static_cast<bool>(fill) && near(fill.quantity, 0.5) &&
                near(order.remaining_quantity, 0.5) && order.active,
            "only volume beyond the remaining queue should partially fill our order");
}

void test_partial_fills_accumulate() {
    auto order = paper::make_resting_order(paper::RestingSide::Buy, 99.5, 1.0, 0.0);

    auto first = paper::apply_trade_print(
        order, paper::TradePrint{99.5, 0.4, paper::AggressorSide::Sell});
    auto second = paper::apply_trade_print(
        order, paper::TradePrint{99.5, 0.6, paper::AggressorSide::Sell});

    require(near(first.quantity, 0.4) && near(second.quantity, 0.6) &&
                near(order.filled_quantity, 1.0) && near(order.remaining_quantity, 0.0) &&
                !order.active,
            "partial fills should accumulate to the order quantity");
}

void test_order_cannot_be_overfilled() {
    auto order = paper::make_resting_order(paper::RestingSide::Sell, 101.0, 0.25, 0.0);
    auto fill = paper::apply_trade_print(
        order, paper::TradePrint{101.0, 10.0, paper::AggressorSide::Buy});
    require(static_cast<bool>(fill) && near(fill.quantity, 0.25) &&
                near(order.filled_quantity, 0.25) && near(order.remaining_quantity, 0.0) &&
                !order.active,
            "a large print must not fill more than the remaining order quantity");
}

void test_invalid_trades_are_ignored() {
    auto order = paper::make_resting_order(paper::RestingSide::Buy, 100.0, 1.0, 0.5);
    const auto nan = std::numeric_limits<double>::quiet_NaN();

    require(!paper::apply_trade_print(
                order, paper::TradePrint{nan, 1.0, paper::AggressorSide::Sell}),
            "NaN price should be ignored");
    require(!paper::apply_trade_print(
                order, paper::TradePrint{100.0, 0.0, paper::AggressorSide::Sell}),
            "zero quantity should be ignored");
    require(near(order.queue_ahead_quantity, 0.5) &&
                near(order.remaining_quantity, 1.0) && near(order.filled_quantity, 0.0),
            "invalid trade should not mutate order state");
}

void test_invalid_order_is_rejected() {
    bool threw = false;
    try {
        (void)paper::make_resting_order(paper::RestingSide::Buy, 0.0, 1.0, 0.0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "non-positive order price should be rejected");

    threw = false;
    try {
        (void)paper::make_resting_order(paper::RestingSide::Buy, 100.0, 1.0, -0.1);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "negative queue-ahead quantity should be rejected");
}

void test_inactive_order_ignores_later_trades() {
    auto order = paper::make_resting_order(paper::RestingSide::Buy, 100.0, 0.2, 0.0);
    auto first = paper::apply_trade_print(
        order, paper::TradePrint{100.0, 0.2, paper::AggressorSide::Sell});
    auto second = paper::apply_trade_print(
        order, paper::TradePrint{100.0, 5.0, paper::AggressorSide::Sell});
    require(static_cast<bool>(first) && !second && near(order.filled_quantity, 0.2),
            "completed order should ignore later prints");
}
} // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"bid direction and price", test_bid_requires_sell_aggressor_at_order_price},
        {"ask direction", test_ask_requires_buy_aggressor},
        {"queue ahead", test_queue_ahead_is_consumed_before_our_fill},
        {"partial fills", test_partial_fills_accumulate},
        {"no overfill", test_order_cannot_be_overfilled},
        {"invalid trade", test_invalid_trades_are_ignored},
        {"invalid order", test_invalid_order_is_rejected},
        {"inactive order", test_inactive_order_ignores_later_trades},
    };

    try {
        for (const auto& [name, test] : tests) {
            test();
            std::cout << "[PASS] " << name << '\n';
        }
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << '\n';
        return 1;
    }

    std::cout << "All " << tests.size() << " trade-print fill-model tests passed.\n";
    return 0;
}