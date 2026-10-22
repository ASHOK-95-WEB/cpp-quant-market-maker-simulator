#pragma once

// Educational paper-trading ledger and pre-trade risk checks.
// Standard C++20 only. This module never sends exchange orders.

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace paper {

enum class Side { Buy, Sell };

struct MarketState {
    double best_bid{0.0};
    double best_ask{0.0};
    bool book_valid{false};
    bool feed_fresh{false};
};

struct OrderIntent {
    Side side{Side::Buy};
    double price{0.0};
    double quantity{0.0};
};

struct RiskConfig {
    double max_order_quantity{0.01};
    double max_long_inventory{0.05};
    double max_loss_usd{25.0};
};

struct PortfolioMark {
    double cash{0.0};
    double inventory{0.0};
    double mark_price{0.0};
    double estimated_exit_fee{0.0};
    double equity{0.0};
    double net_pnl{0.0};
    double realised_net_pnl{0.0};
    double unrealised_net_pnl{0.0};
    double fees_paid{0.0};
    double peak_equity{0.0};
    double current_drawdown{0.0};
    double maximum_drawdown{0.0};
};

class Ledger {
public:
    Ledger(double initial_cash, double fee_rate)
        : initial_cash_(initial_cash), cash_(initial_cash), fee_rate_(fee_rate),
          peak_equity_(initial_cash) {
        if (!std::isfinite(initial_cash) || initial_cash <= 0.0)
            throw std::invalid_argument("Initial cash must be finite and positive");
        if (!std::isfinite(fee_rate) || fee_rate < 0.0 || fee_rate >= 1.0)
            throw std::invalid_argument("Fee rate must be finite and in [0, 1)");
    }

    // Record an already simulated fill. Risk checks should be performed first.
    // Spot-only ledger: it does not permit short positions.
    double apply_fill(Side side, double price, double quantity) {
        if (!std::isfinite(price) || price <= 0.0)
            throw std::invalid_argument("Fill price must be finite and positive");
        if (!std::isfinite(quantity) || quantity <= 0.0)
            throw std::invalid_argument("Fill quantity must be finite and positive");

        const double notional = price * quantity;
        if (!std::isfinite(notional) || notional <= 0.0)
            throw std::invalid_argument("Invalid fill notional");
        const double fee = notional * fee_rate_;

        if (side == Side::Buy) {
            if (cash_ + 1e-9 < notional + fee)
                throw std::runtime_error("Insufficient virtual cash for buy fill");
            const double new_inventory = inventory_ + quantity;
            average_cost_per_btc_ =
                (inventory_ * average_cost_per_btc_ + notional + fee) / new_inventory;
            cash_ -= notional + fee;
            inventory_ = new_inventory;
        } else {
            if (quantity > inventory_ + 1e-9)
                throw std::runtime_error("Sell fill would create a short position");
            const double net_proceeds = notional - fee;
            const double cost_basis_released = average_cost_per_btc_ * quantity;
            const double trade_net = net_proceeds - cost_basis_released;
            cash_ += net_proceeds;
            inventory_ -= quantity;
            realised_net_pnl_ += trade_net;
            if (inventory_ < 1e-9) {
                inventory_ = 0.0;
                average_cost_per_btc_ = 0.0;
            }
        }

        fees_paid_ += fee;
        return fee;
    }

    // Marks a long spot position at best_bid and reserves an estimated exit fee.
    // This is a conservative liquidation-value estimate, not a guaranteed exit price.
    PortfolioMark mark_to_market(double best_bid) {
        if (!std::isfinite(best_bid) || best_bid <= 0.0)
            throw std::invalid_argument("Mark price must be finite and positive");

        const double exit_fee = inventory_ * best_bid * fee_rate_;
        const double equity = cash_ + inventory_ * best_bid - exit_fee;
        peak_equity_ = std::max(peak_equity_, equity);
        const double drawdown = peak_equity_ - equity;
        maximum_drawdown_ = std::max(maximum_drawdown_, drawdown);

        PortfolioMark mark;
        mark.cash = cash_;
        mark.inventory = inventory_;
        mark.mark_price = best_bid;
        mark.estimated_exit_fee = exit_fee;
        mark.equity = equity;
        mark.net_pnl = equity - initial_cash_;
        mark.realised_net_pnl = realised_net_pnl_;
        mark.unrealised_net_pnl = mark.net_pnl - realised_net_pnl_;
        mark.fees_paid = fees_paid_;
        mark.peak_equity = peak_equity_;
        mark.current_drawdown = drawdown;
        mark.maximum_drawdown = maximum_drawdown_;
        return mark;
    }

    double initial_cash() const { return initial_cash_; }
    double cash() const { return cash_; }
    double inventory() const { return inventory_; }
    double average_cost_per_btc() const { return average_cost_per_btc_; }
    double realised_net_pnl() const { return realised_net_pnl_; }
    double fees_paid() const { return fees_paid_; }
    double fee_rate() const { return fee_rate_; }

private:
    double initial_cash_;
    double cash_;
    double fee_rate_;
    double inventory_{0.0};
    // Includes buy-side fees allocated to the remaining inventory.
    double average_cost_per_btc_{0.0};
    double realised_net_pnl_{0.0};
    double fees_paid_{0.0};
    double peak_equity_;
    double maximum_drawdown_{0.0};
};

struct RiskDecision {
    bool allowed{false};
    std::string reason;
};

class RiskManager {
public:
    explicit RiskManager(RiskConfig config) : config_(config) {
        if (!std::isfinite(config_.max_order_quantity) || config_.max_order_quantity <= 0.0)
            throw std::invalid_argument("max_order_quantity must be positive");
        if (!std::isfinite(config_.max_long_inventory) || config_.max_long_inventory <= 0.0)
            throw std::invalid_argument("max_long_inventory must be positive");
        if (!std::isfinite(config_.max_loss_usd) || config_.max_loss_usd <= 0.0)
            throw std::invalid_argument("max_loss_usd must be positive");
    }

    RiskDecision check(const OrderIntent& order, const MarketState& market, Ledger& ledger) {
        if (halted_) return reject("Trading halted by the risk kill switch");
        if (!market.feed_fresh) return reject("Market data is stale");
        if (!market.book_valid) return reject("Local order book is not synchronized/valid");
        if (!std::isfinite(market.best_bid) || !std::isfinite(market.best_ask) ||
            market.best_bid <= 0.0 || market.best_ask <= market.best_bid)
            return reject("Invalid or crossed market");

        const PortfolioMark mark = ledger.mark_to_market(market.best_bid);
        if (mark.net_pnl <= -config_.max_loss_usd) {
            halted_ = true; // Latched until the process is deliberately restarted/re-armed.
            return reject("Maximum loss limit reached; kill switch latched");
        }

        if (!std::isfinite(order.price) || order.price <= 0.0)
            return reject("Order price must be finite and positive");
        if (!std::isfinite(order.quantity) || order.quantity <= 0.0)
            return reject("Order quantity must be finite and positive");
        if (order.quantity > config_.max_order_quantity + 1e-12)
            return reject("Order exceeds maximum quantity");

        if (order.side == Side::Buy) {
            if (order.price >= market.best_ask)
                return reject("Buy quote would cross the ask; expected a resting maker quote");
            if (ledger.inventory() + order.quantity > config_.max_long_inventory + 1e-12)
                return reject("Buy would exceed maximum long inventory");
            const double required_cash = order.price * order.quantity * (1.0 + ledger.fee_rate());
            if (ledger.cash() + 1e-9 < required_cash)
                return reject("Insufficient virtual cash including fee");
        } else {
            if (order.price <= market.best_bid)
                return reject("Sell quote would cross the bid; expected a resting maker quote");
            if (order.quantity > ledger.inventory() + 1e-12)
                return reject("Sell would create a short position");
        }

        return {true, "Accepted by paper risk checks"};
    }

    bool halted() const { return halted_; }

private:
    static RiskDecision reject(std::string reason) {
        return {false, std::move(reason)};
    }

    RiskConfig config_;
    bool halted_{false};
};

} // namespace paper
