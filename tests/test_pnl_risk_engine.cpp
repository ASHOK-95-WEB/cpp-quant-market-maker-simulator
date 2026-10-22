#include "../src/live/paper/pnl_risk_engine.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

using paper::Ledger;
using paper::MarketState;
using paper::OrderIntent;
using paper::RiskConfig;
using paper::RiskManager;
using paper::Side;

namespace {
int passed = 0;

void expect(bool condition, const std::string& name) {
    if (!condition) throw std::runtime_error("FAIL: " + name);
    ++passed;
    std::cout << "PASS: " << name << '\n';
}

bool near(double a, double b, double eps = 1e-8) {
    return std::abs(a - b) <= eps;
}

MarketState good_market(double bid = 100.0, double ask = 100.01) {
    return {bid, ask, true, true};
}
}

int main() {
    try {
        // 1. Buy fill includes buy-side fee in cash and average cost basis.
        Ledger ledger(1000.0, 0.001);
        const double buy_fee = ledger.apply_fill(Side::Buy, 100.0, 1.0);
        expect(near(buy_fee, 0.1), "buy fee calculated");
        expect(near(ledger.cash(), 899.9), "buy reduces cash by notional plus fee");
        expect(near(ledger.inventory(), 1.0), "buy increases inventory");
        expect(near(ledger.average_cost_per_btc(), 100.1), "buy fee included in cost basis");

        // 2. Sell fill includes both entry and exit fees in realised net P&L.
        const double sell_fee = ledger.apply_fill(Side::Sell, 101.0, 1.0);
        expect(near(sell_fee, 0.101), "sell fee calculated");
        expect(near(ledger.realised_net_pnl(), 0.799), "realised net P&L includes both fees");
        expect(near(ledger.cash(), 1000.799), "cash reconciles after round trip");
        expect(near(ledger.inventory(), 0.0), "inventory closes to zero");
        const auto closed = ledger.mark_to_market(100.5);
        expect(near(closed.net_pnl, 0.799), "closed-position equity change equals net P&L");

        // 3. Open-position mark reserves estimated exit fees.
        Ledger open(1000.0, 0.001);
        open.apply_fill(Side::Buy, 100.0, 1.0);
        const auto open_mark = open.mark_to_market(100.0);
        expect(near(open_mark.equity, 999.8), "open mark reserves estimated liquidation fee");
        expect(near(open_mark.net_pnl, -0.2), "open net P&L reflects entry and estimated exit fee");

        // 4. Ledger prevents shorts and bad fills.
        bool short_rejected = false;
        try { open.apply_fill(Side::Sell, 100.0, 2.0); }
        catch (const std::runtime_error&) { short_rejected = true; }
        expect(short_rejected, "ledger rejects sale exceeding inventory");
        bool zero_rejected = false;
        try { open.apply_fill(Side::Buy, 100.0, 0.0); }
        catch (const std::invalid_argument&) { zero_rejected = true; }
        expect(zero_rejected, "ledger rejects zero-quantity fill");

        // 5. A modest resting buy quote is allowed by risk limits.
        Ledger rledger(1000.0, 0.001);
        RiskManager risk({0.02, 0.05, 50.0});
        const auto accepted = risk.check({Side::Buy, 99.99, 0.01}, good_market(), rledger);
        expect(accepted.allowed, "valid resting buy quote accepted");

        // 6. Pre-trade guard rails.
        expect(!risk.check({Side::Buy, 99.99, 0.03}, good_market(), rledger).allowed,
               "order quantity limit enforced");
        expect(!risk.check({Side::Buy, 99.99, 0.06}, good_market(), rledger).allowed,
               "maximum inventory limit enforced");
        expect(!risk.check({Side::Buy, 100.01, 0.01}, good_market(), rledger).allowed,
               "marketable buy quote rejected");
        expect(!risk.check({Side::Sell, 100.02, 0.01}, good_market(), rledger).allowed,
               "sell quote without inventory rejected");
        auto stale = good_market(); stale.feed_fresh = false;
        expect(!risk.check({Side::Buy, 99.99, 0.01}, stale, rledger).allowed,
               "stale market data blocks new orders");
        auto invalid = good_market(); invalid.book_valid = false;
        expect(!risk.check({Side::Buy, 99.99, 0.01}, invalid, rledger).allowed,
               "invalid local book blocks new orders");

        // 7. Kill switch latches after net liquidation P&L exceeds loss limit.
        Ledger loss_ledger(1000.0, 0.001);
        loss_ledger.apply_fill(Side::Buy, 100.0, 0.1);
        RiskManager loss_risk({0.02, 0.2, 5.0});
        const auto halted = loss_risk.check({Side::Buy, 49.99, 0.01}, good_market(50.0, 50.01), loss_ledger);
        expect(!halted.allowed && loss_risk.halted(), "maximum-loss kill switch latches");
        expect(!loss_risk.check({Side::Buy, 49.99, 0.01}, good_market(50.0, 50.01), loss_ledger).allowed,
               "latched kill switch blocks subsequent orders");

        std::cout << "\nAll " << passed << " P&L/risk tests passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
