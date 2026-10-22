# Integrated Binance Paper Market Maker (C++20)

## What this adds

This step connects the synchronized local order book to the standard-C++20 paper ledger and pre-trade risk manager in one executable. It consumes public Binance BTC/USDT market data only and does not use a Binance account, API key, or trading endpoint.

Included files (extract over the project root):

- `src/live/integrated_paper_market_maker.cpp` — integrated feed, quote logic, virtual fills, ledger and risk checks.
- `src/live/local_order_book.hpp` — local order-book model used by the existing feed monitor.
- `src/live/paper/pnl_risk_engine.hpp` — previously tested ledger and risk manager.

The two header files are included so the bundle is reproducible. They match the modules previously added in the project; keep the already committed copies if you have made your own edits to them.

## Build on Windows in MSYS2 UCRT64

Open **MSYS2 UCRT64** (not PowerShell), then run:

```bash
cd "/c/Users/Ashok Chorghade/Documents/quant-lob-week1/quant-lob"
g++ -std=c++20 -O2 -Wall -Wextra src/live/integrated_paper_market_maker.cpp -o integrated_paper_market_maker.exe $(pkg-config --cflags --libs libcurl) -lssl -lcrypto -lws2_32 -lcrypt32
```

Run it from the project root:

```bash
./integrated_paper_market_maker.exe
```

Stop it with **Ctrl+C**. It writes rows to `data/integrated_paper_trader.csv`. CSV output is local research data and should not be committed to Git.

## Initial illustrative settings

| Setting | Value |
|---|---:|
| Starting virtual cash | $10,000 |
| Assumed fee per fill | 0.10% |
| Paper order quantity | 0.001 BTC |
| Maximum long inventory | 0.005 BTC |
| Maximum net liquidation loss | $25 |
| Additional target edge beyond the fee-aware quote width | 1.5 bps per side |

These are educational assumptions. The fee rate is **not** a claim about your actual Binance fees. The risk kill switch is latched for the life of the process after the loss threshold is reached.

## How it works

1. Connect to Binance's public combined incremental-depth and aggregate-trade WebSocket streams.
2. Buffer depth messages and obtain a public REST depth snapshot.
3. Ignore messages already covered by the snapshot; the first newer event must bridge the snapshot update ID. Any detected update gap ends that session and starts a fresh snapshot sync.
4. Calculate mid-price, microprice and five-level depth imbalance.
5. Calculate a fee-aware bid/ask around a simple microprice/imbalance-adjusted fair-value estimate.
6. Run each new quote through `RiskManager`, including the loss kill switch, quantity limit, inventory limit, stale-feed flag and market-validity checks.
7. Simulate fills, account for the illustrative fee, mark inventory conservatively at best bid less an estimated exit fee, and record net P&L/drawdown.

## Critical modelling limits

- **This is not real HFT and it does not guarantee profits.** It is a local paper-trading research prototype.
- Fills are attributed only when a public Binance `aggTrade` print matches the hypothetical limit price and aggressor direction, after estimated same-price displayed queue volume is consumed. This remains approximate: aggregate public prints do not reveal our actual queue priority, hidden liquidity, or all cancellations. When no same-price level is visible, queue-ahead estimate defaults to zero and may be optimistic. `data/trade_print_fill_events.csv` records the trade-print stream and estimated fills separately from the book-state log. Simulated fill rate and P&L are not evidence of executable profitability.
- Quote widths include the configured fee assumption plus an extra edge. With a 0.10% assumed fee per fill, these quotes may be far from the market and rarely fill; that is preferable to pretending costs are zero.
- Local read timestamps are not exchange-to-computer latency measurements. Clock skew, network delay and synchronous buffering matter.
- The book stores prices as `double`, and the REST snapshot contains a limited number of levels. A production-quality implementation should use integer ticks/lot units from exchange metadata, asynchronous bounded buffering, richer recovery logic, trade/queue modelling, slippage, and independent replay/backtest validation.
- Always evaluate net liquidation P&L after fees, spread crossing, slippage, and realistic fill assumptions; never optimize for gross P&L alone.

## Validation

The previously supplied unit tests for `LocalOrderBook` and `Ledger`/`RiskManager` should still be run after integrating:

```bash
g++ -std=c++20 -O2 -Wall -Wextra tests/test_local_order_book.cpp -o test_local_order_book.exe
./test_local_order_book.exe

g++ -std=c++20 -O2 -Wall -Wextra tests/test_pnl_risk_engine.cpp -o test_pnl_risk_engine.exe
./test_pnl_risk_engine.exe
```

After applying the trade-print integration, rebuild all targets with `cmake --build build --parallel 2` and run `ctest --test-dir build --output-on-failure`. Verify that the executable connects, writes the trade-print CSV, and reports any simulated fills before interpreting its results. Successful compilation or paper fills do not establish profitability.
