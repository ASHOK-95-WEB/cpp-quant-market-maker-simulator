#pragma once

#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

struct PriceLevelUpdate {
    double price{};
    double quantity{}; // absolute quantity; zero means remove this level
};

struct DepthEvent {
    std::uint64_t first_update_id{}; // Binance U
    std::uint64_t final_update_id{}; // Binance u
    std::int64_t event_time_ms{};    // Binance E, when present
    std::vector<PriceLevelUpdate> bids;
    std::vector<PriceLevelUpdate> asks;
};

enum class ApplyResult {
    Applied,
    IgnoredOld,
    GapDetected
};

class LocalOrderBook {
public:
    using BidMap = std::map<double, double, std::greater<double>>;
    using AskMap = std::map<double, double>;

    void load_snapshot(std::uint64_t last_update_id,
                       const std::vector<PriceLevelUpdate>& bids,
                       const std::vector<PriceLevelUpdate>& asks) {
        bids_.clear();
        asks_.clear();
        apply_levels(bids_, bids);
        apply_levels(asks_, asks);
        last_update_id_ = last_update_id;
        initialized_ = true;
    }

    ApplyResult apply(const DepthEvent& event) {
        if (!initialized_) return ApplyResult::GapDetected;

        // Events fully covered by the snapshot/current book are stale.
        if (event.final_update_id <= last_update_id_)
            return ApplyResult::IgnoredOld;

        // If the event starts beyond the next expected update, data is missing.
        if (event.first_update_id > last_update_id_ + 1)
            return ApplyResult::GapDetected;

        apply_levels(bids_, event.bids);
        apply_levels(asks_, event.asks);
        last_update_id_ = event.final_update_id;
        return ApplyResult::Applied;
    }

    bool initialized() const { return initialized_; }
    std::uint64_t last_update_id() const { return last_update_id_; }
    bool has_bids() const { return !bids_.empty(); }
    bool has_asks() const { return !asks_.empty(); }
    bool has_two_sided_book() const { return has_bids() && has_asks(); }

    double best_bid() const { return bids_.begin()->first; }
    double best_bid_qty() const { return bids_.begin()->second; }
    double best_ask() const { return asks_.begin()->first; }
    double best_ask_qty() const { return asks_.begin()->second; }

    // Estimate pre-existing displayed quantity at an exact hypothetical order price.
    // A missing level returns zero; hidden liquidity and the true queue remain unknown.
    double bid_quantity_at_price(double price, double tolerance = 0.001) const {
        for (const auto& [level_price, quantity] : bids_) {
            if (std::abs(level_price - price) <= tolerance) return quantity;
        }
        return 0.0;
    }

    double ask_quantity_at_price(double price, double tolerance = 0.001) const {
        for (const auto& [level_price, quantity] : asks_) {
            if (std::abs(level_price - price) <= tolerance) return quantity;
        }
        return 0.0;
    }

    bool valid_market() const {
        return has_two_sided_book() && best_bid() < best_ask();
    }

    double spread() const { return best_ask() - best_bid(); }
    double mid_price() const { return (best_bid() + best_ask()) / 2.0; }

    double microprice() const {
        const double total = best_bid_qty() + best_ask_qty();
        if (total <= 0.0) return mid_price();
        return (best_ask() * best_bid_qty() + best_bid() * best_ask_qty()) / total;
    }

    double depth_imbalance(std::size_t levels = 5) const {
        double bid_qty = 0.0;
        double ask_qty = 0.0;
        std::size_t n = 0;
        for (const auto& [price, quantity] : bids_) {
            (void)price;
            if (n++ >= levels) break;
            bid_qty += quantity;
        }
        n = 0;
        for (const auto& [price, quantity] : asks_) {
            (void)price;
            if (n++ >= levels) break;
            ask_qty += quantity;
        }
        const double total = bid_qty + ask_qty;
        return total > 0.0 ? (bid_qty - ask_qty) / total : 0.0;
    }

private:
    template <typename Map>
    static void apply_levels(Map& book, const std::vector<PriceLevelUpdate>& levels) {
        for (const auto& level : levels) {
            if (level.price <= 0.0 || level.quantity < 0.0) continue;
            if (level.quantity == 0.0) {
                book.erase(level.price);
            } else {
                book[level.price] = level.quantity;
            }
        }
    }

    BidMap bids_;
    AskMap asks_;
    std::uint64_t last_update_id_{};
    bool initialized_{};
};
