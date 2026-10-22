$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath (Join-Path $PWD 'CMakeLists.txt') -PathType Leaf)) {
    throw 'Run this script from the quant-lob repository root (the folder containing CMakeLists.txt).'
}

$headerPath = Join-Path $PWD 'src\live\paper\trade_print_fill_model.hpp'
$testPath = Join-Path $PWD 'tests\test_trade_print_fill_model.cpp'
if ((Test-Path -LiteralPath $headerPath) -or (Test-Path -LiteralPath $testPath)) {
    throw 'At least one new trade-print model file already exists. No files were overwritten; inspect the existing files first.'
}

$header = @'
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
'@

$test = @'
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
'@

$cmakePath = Join-Path $PWD 'CMakeLists.txt'
$cmake = [System.IO.File]::ReadAllText($cmakePath)
$anchor = 'add_test(NAME pnl_risk_engine COMMAND test_pnl_risk_engine)'
if (-not $cmake.Contains($anchor)) {
    throw 'Could not find the expected P&L/risk CTest registration. No new files have been written.'
}
if ($cmake.Contains('test_trade_print_fill_model')) {
    throw 'CMakeLists.txt already contains a trade-print fill-model target. No new files have been written.'
}

$addition = @'
add_test(NAME pnl_risk_engine COMMAND test_pnl_risk_engine)

# Isolated unit-tested approximation for passive fills inferred from trade prints.
add_executable(test_trade_print_fill_model tests/test_trade_print_fill_model.cpp)
target_include_directories(test_trade_print_fill_model PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/live/paper"
)
quant_enable_warnings(test_trade_print_fill_model)
add_test(NAME trade_print_fill_model COMMAND test_trade_print_fill_model)
'@

# Write the new source files only after all preconditions are checked.
$ascii = [System.Text.Encoding]::ASCII
[System.IO.File]::WriteAllText($headerPath, $header, $ascii)
[System.IO.File]::WriteAllText($testPath, $test, $ascii)
$cmake = $cmake.Replace($anchor, $addition.TrimEnd())
[System.IO.File]::WriteAllText($cmakePath, $cmake, $ascii)

Write-Host 'Created src/live/paper/trade_print_fill_model.hpp'
Write-Host 'Created tests/test_trade_print_fill_model.cpp'
Write-Host 'Registered trade_print_fill_model in CMake/CTest'
Write-Host 'No existing source files were overwritten.'
