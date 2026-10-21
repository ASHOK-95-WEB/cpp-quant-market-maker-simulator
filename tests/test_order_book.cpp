#include <iostream>

#include "order_book.hpp"

using namespace lob;

static int failures = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::cerr << "FAIL line " << __LINE__ << ": " #cond << "\n";   \
            ++failures;                                                    \
        }                                                                  \
    } while (0)

static void test_rest_and_best_prices() {
    OrderBook b;
    CHECK(b.add_limit(1, Side::Buy, 100, 10).empty());
    CHECK(b.add_limit(2, Side::Sell, 102, 5).empty());
    CHECK(b.best_bid() == 100);
    CHECK(b.best_ask() == 102);
    CHECK(b.order_count() == 2);
}

static void test_full_match() {
    OrderBook b;
    b.add_limit(1, Side::Buy, 100, 10);
    auto t = b.add_limit(2, Side::Sell, 100, 10);
    CHECK(t.size() == 1);
    CHECK(t[0].taker_id == 2 && t[0].maker_id == 1);
    CHECK(t[0].price == 100 && t[0].qty == 10);
    CHECK(!b.best_bid().has_value());
    CHECK(!b.best_ask().has_value());
    CHECK(b.order_count() == 0);
}

static void test_partial_fill_rests_remainder() {
    OrderBook b;
    b.add_limit(1, Side::Sell, 101, 5);
    auto t = b.add_limit(2, Side::Buy, 101, 8);
    CHECK(t.size() == 1 && t[0].qty == 5);
    CHECK(!b.best_ask().has_value());
    CHECK(b.best_bid() == 101);
    CHECK(b.depth_at(Side::Buy, 101) == 3);
}

static void test_time_priority_at_same_price() {
    OrderBook b;
    b.add_limit(1, Side::Sell, 101, 5);
    b.add_limit(2, Side::Sell, 101, 5);
    auto t1 = b.add_limit(3, Side::Buy, 101, 5);
    CHECK(t1.size() == 1 && t1[0].maker_id == 1);  // oldest fills first
    auto t2 = b.add_limit(4, Side::Buy, 101, 5);
    CHECK(t2.size() == 1 && t2[0].maker_id == 2);
}

static void test_price_priority() {
    OrderBook b;
    b.add_limit(1, Side::Sell, 102, 5);
    b.add_limit(2, Side::Sell, 101, 5);  // better price, arrived later
    auto t = b.add_limit(3, Side::Buy, 102, 5);
    CHECK(t.size() == 1 && t[0].maker_id == 2 && t[0].price == 101);
}

static void test_trade_at_resting_price() {
    OrderBook b;
    b.add_limit(1, Side::Sell, 100, 5);
    auto t = b.add_limit(2, Side::Buy, 105, 5);  // willing to pay 105
    CHECK(t.size() == 1 && t[0].price == 100);   // but trades at 100
}

static void test_sweep_multiple_levels() {
    OrderBook b;
    b.add_limit(1, Side::Sell, 101, 5);
    b.add_limit(2, Side::Sell, 102, 5);
    auto t = b.add_limit(3, Side::Buy, 102, 8);
    CHECK(t.size() == 2);
    CHECK(t[0].price == 101 && t[0].qty == 5);
    CHECK(t[1].price == 102 && t[1].qty == 3);
    CHECK(b.best_ask() == 102);
    CHECK(b.depth_at(Side::Sell, 102) == 2);
}

static void test_no_cross_when_prices_dont_meet() {
    OrderBook b;
    b.add_limit(1, Side::Sell, 102, 5);
    auto t = b.add_limit(2, Side::Buy, 101, 5);
    CHECK(t.empty());
    CHECK(b.best_bid() == 101 && b.best_ask() == 102);
}

static void test_cancel() {
    OrderBook b;
    b.add_limit(1, Side::Buy, 100, 10);
    CHECK(b.cancel(1));
    CHECK(!b.cancel(1));  // second cancel fails
    CHECK(!b.best_bid().has_value());
    CHECK(b.order_count() == 0);
}

static void test_cancel_after_fill() {
    OrderBook b;
    b.add_limit(1, Side::Sell, 100, 5);
    b.add_limit(2, Side::Buy, 100, 5);
    CHECK(!b.cancel(1));  // already filled
}

static void test_cancel_middle_of_queue_keeps_priority() {
    OrderBook b;
    b.add_limit(1, Side::Sell, 101, 5);
    b.add_limit(2, Side::Sell, 101, 5);
    b.add_limit(3, Side::Sell, 101, 5);
    CHECK(b.cancel(2));
    auto t = b.add_limit(4, Side::Buy, 101, 10);
    CHECK(t.size() == 2 && t[0].maker_id == 1 && t[1].maker_id == 3);
}

static void test_invalid_input_rejected() {
    OrderBook b;
    CHECK(b.add_limit(1, Side::Buy, 100, 0).empty());
    CHECK(b.add_limit(2, Side::Buy, 100, -5).empty());
    CHECK(b.order_count() == 0);
    b.add_limit(3, Side::Buy, 100, 5);
    b.add_limit(3, Side::Buy, 100, 5);  // duplicate id
    CHECK(b.order_count() == 1);
    CHECK(b.depth_at(Side::Buy, 100) == 5);
}

int main() {
    test_rest_and_best_prices();
    test_full_match();
    test_partial_fill_rests_remainder();
    test_time_priority_at_same_price();
    test_price_priority();
    test_trade_at_resting_price();
    test_sweep_multiple_levels();
    test_no_cross_when_prices_dont_meet();
    test_cancel();
    test_cancel_after_fill();
    test_cancel_middle_of_queue_keeps_priority();
    test_invalid_input_rejected();

    if (failures == 0) {
        std::cout << "All tests passed\n";
        return 0;
    }
    std::cerr << failures << " check(s) failed\n";
    return 1;
}
