#include "../src/live/local_order_book.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

static bool near(double a, double b, double tolerance = 1e-9) {
    return std::abs(a - b) <= tolerance;
}

int main() {
    LocalOrderBook book;
    book.load_snapshot(100,
        {{100.0, 2.0}, {99.0, 3.0}},
        {{101.0, 4.0}, {102.0, 5.0}});

    assert(book.initialized());
    assert(book.valid_market());
    assert(book.last_update_id() == 100);
    assert(near(book.best_bid(), 100.0));
    assert(near(book.best_ask(), 101.0));
    assert(near(book.spread(), 1.0));
    assert(near(book.mid_price(), 100.5));

    // Old update must not change the book.
    DepthEvent old_event{90, 100, 0, {{100.0, 999.0}}, {}};
    assert(book.apply(old_event) == ApplyResult::IgnoredOld);
    assert(near(book.best_bid_qty(), 2.0));
    assert(book.last_update_id() == 100);

    // Event overlaps/bridges the next expected update (101) and updates absolute quantities.
    DepthEvent good_event{101, 103, 12345,
        {{100.0, 1.5}, {98.0, 7.0}},
        {{101.0, 0.0}, {100.5, 2.0}}};
    assert(book.apply(good_event) == ApplyResult::Applied);
    assert(book.last_update_id() == 103);
    assert(near(book.best_bid(), 100.0));
    assert(near(book.best_bid_qty(), 1.5));
    assert(near(book.best_ask(), 100.5));

    // Zero quantity removes a price level.
    DepthEvent remove_event{104, 104, 12346, {{100.0, 0.0}}, {{100.5, 0.0}}};
    assert(book.apply(remove_event) == ApplyResult::Applied);
    assert(book.last_update_id() == 104);
    assert(near(book.best_bid(), 99.0));
    assert(near(book.best_ask(), 102.0));

    // A skipped update range must be flagged; state must not advance.
    DepthEvent gap_event{106, 108, 12347, {{99.0, 1.0}}, {}};
    assert(book.apply(gap_event) == ApplyResult::GapDetected);
    assert(book.last_update_id() == 104);
    assert(near(book.best_bid_qty(), 3.0));

    // A continuous update applies normally.
    DepthEvent next_event{105, 105, 12348, {{99.0, 4.0}}, {}};
    assert(book.apply(next_event) == ApplyResult::Applied);
    assert(book.last_update_id() == 105);
    assert(near(book.best_bid_qty(), 4.0));

    std::cout << "All local order-book tests passed.\n";
    return 0;
}
