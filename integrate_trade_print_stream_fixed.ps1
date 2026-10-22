$ErrorActionPreference = 'Stop'

$root = (Get-Location).Path
$mainPath = Join-Path $root 'src\live\integrated_paper_market_maker.cpp'
$bookPath = Join-Path $root 'src\live\local_order_book.hpp'
$gitignorePath = Join-Path $root '.gitignore'
$readmePath = Join-Path $root 'README_INTEGRATED_PAPER_TRADER.md'
$generalReadmePath = Join-Path $root 'README.md'
$localTestPath = Join-Path $root 'tests\test_local_order_book.cpp'
$modelPath = Join-Path $root 'src\live\paper\trade_print_fill_model.hpp'
$testPath = Join-Path $root 'tests\test_trade_print_fill_model.cpp'

if (-not (Test-Path -LiteralPath (Join-Path $root 'CMakeLists.txt') -PathType Leaf)) {
    throw 'Run this script from the quant-lob repository root.'
}
foreach ($required in @($mainPath, $bookPath, $modelPath, $testPath, $gitignorePath, $readmePath, $generalReadmePath, $localTestPath)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "Required file missing: $required" }
}

$utf8NoBom = [System.Text.UTF8Encoding]::new($false)
function Read-Normalized([string]$Path) {
    $text = [System.IO.File]::ReadAllText($Path)
    return $text.Replace("`r`n", "`n")
}
function Write-Normalized([string]$Path, [string]$Text) {
    [System.IO.File]::WriteAllText($Path, $Text.Replace("`r`n", "`n"), $utf8NoBom)
}
function Replace-Once([string]$Source, [string]$Old, [string]$New, [string]$Label) {
    $Old = $Old.Replace("`r`n", "`n")
    $New = $New.Replace("`r`n", "`n")
    $count = ([regex]::Matches($Source, [regex]::Escape($Old))).Count
    if ($count -ne 1) { throw "Expected exactly one match for '$Label'; found $count. No files have been written yet." }
    return $Source.Replace($Old, $New)
}

$main = Read-Normalized $mainPath
$book = Read-Normalized $bookPath
$gitignore = Read-Normalized $gitignorePath
$readme = Read-Normalized $readmePath
$generalReadme = Read-Normalized $generalReadmePath
$localTest = Read-Normalized $localTestPath

if ($main.Contains('/stream?streams=btcusdt@depth@100ms/btcusdt@aggTrade')) {
    throw 'The integrated market maker already appears to use the combined depth/aggTrade stream. No changes were made.'
}
if (-not ((Read-Normalized (Join-Path $root 'CMakeLists.txt')).Contains('test_trade_print_fill_model'))) {
    throw 'The trade_print_fill_model test target is not registered in CMake. Run the setup script first.'
}

# Extend the local book with same-price displayed quantity lookups for queue estimates.
$book = Replace-Once $book '#include <cstdint>
#include <functional>
#include <map>' '#include <cmath>
#include <cstdint>
#include <functional>
#include <map>' 'local-book cmath include'
$oldText1 = @'
    double best_bid() const { return bids_.begin()->first; }
    double best_bid_qty() const { return bids_.begin()->second; }
    double best_ask() const { return asks_.begin()->first; }
    double best_ask_qty() const { return asks_.begin()->second; }
'@
$newText1 = @'
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
'@
$book = Replace-Once $book $oldText1 $newText1 'local-book queue accessors'

# Add trade-print parsing and switch to Binance's combined market-data stream.
$main = Replace-Once $main '#include "paper/pnl_risk_engine.hpp"' '#include "paper/pnl_risk_engine.hpp"
#include "paper/trade_print_fill_model.hpp"' 'trade-print include'
$main = Replace-Once $main 'constexpr const char* kWsTarget = "/ws/btcusdt@depth@100ms";' 'constexpr const char* kWsTarget = "/stream?streams=btcusdt@depth@100ms/btcusdt@aggTrade";' 'combined stream target'
$oldText2 = @'
struct WorkingOrder {
    double price{0.0};
    double quantity{0.0};
    bool active{false};
};
'@
$newText2 = @'
struct WorkingOrder {
    double price{0.0};
    double quantity{0.0}; // remaining quantity
    bool active{false};
    double queue_ahead_quantity{0.0}; // estimated displayed quantity at this exact price
};
'@
$main = Replace-Once $main $oldText2 $newText2 'working order queue field'
$oldText3 = @'
bool is_depth_event(const json& value) {
    return value.is_object() && value.value("e", std::string{}) == "depthUpdate" &&
           value.contains("U") && value.contains("u") &&
           value.contains("b") && value.contains("a");
}
'@
$newText3 = @'
bool is_depth_event(const json& value) {
    return value.is_object() && value.value("e", std::string{}) == "depthUpdate" &&
           value.contains("U") && value.contains("u") &&
           value.contains("b") && value.contains("a");
}

bool is_agg_trade_event(const json& value) {
    return value.is_object() && value.value("e", std::string{}) == "aggTrade" &&
           value.contains("p") && value.contains("q") && value.contains("m");
}

paper::TradePrint parse_trade_print(const json& value) {
    const bool buyer_is_maker = value.at("m").get<bool>();
    const paper::AggressorSide aggressor = buyer_is_maker
        ? paper::AggressorSide::Sell
        : paper::AggressorSide::Buy;
    return paper::TradePrint{
        json_number(value.at("p")), json_number(value.at("q")), aggressor};
}
'@
$main = Replace-Once $main $oldText3 $newText3 'aggTrade recognition and parser'
$oldText4 = @'
    json payload = json::parse(beast::buffers_to_string(buffer.data()));
    return ReceivedMessage{std::move(payload), local_ms};
'@
$newText4 = @'
    json payload = json::parse(beast::buffers_to_string(buffer.data()));
    // Combined Binance streams wrap each raw event in {"stream":"...","data":{...}}.
    if (payload.is_object() && payload.contains("data") && payload.at("data").is_object())
        payload = payload.at("data");
    return ReceivedMessage{std::move(payload), local_ms};
'@
$main = Replace-Once $main $oldText4 $newText4 'combined stream unwrapping'

# Replace the old depth-touch fill trigger with the public trade-print/queue model.
$startMarker = '// Order-book-only touch model: intentionally an approximation, not a queue-position simulator.'
$endMarker = 'void run_one_session(paper::Ledger& ledger, paper::RiskManager& risk, std::ofstream& csv) {'
$start = $main.IndexOf($startMarker)
if ($start -lt 0) { throw 'Could not find the old book-touch simulator. No files have been written yet.' }
$end = $main.IndexOf($endMarker, $start)
if ($end -lt 0 -or $main.IndexOf($startMarker, $start + 1) -ge 0) {
    throw 'Could not uniquely locate the old book-touch simulator. No files have been written yet.'
}
$newModel = @'
paper::RestingOrder to_model_order(const WorkingOrder& order, paper::RestingSide side) {
    return paper::RestingOrder{side, order.price, order.quantity, order.quantity,
                               order.queue_ahead_quantity, 0.0, order.active};
}

void store_model_order(WorkingOrder& target, const paper::RestingOrder& source) {
    target.quantity = source.remaining_quantity;
    target.queue_ahead_quantity = source.queue_ahead_quantity;
    target.active = source.active;
}

// Fills are inferred only from public aggTrade prints at the hypothetical limit price.
// Same-price displayed depth is an estimate of queue ahead, not exchange queue certainty.
SimulatedFill try_trade_print_fill(const paper::TradePrint& trade,
                                   const LocalOrderBook& book,
                                   WorkingOrder& bid_order,
                                   WorkingOrder& ask_order,
                                   paper::Ledger& ledger,
                                   paper::RiskManager& risk,
                                   bool feed_fresh,
                                   std::string& risk_status) {
    if (risk.halted()) {
        bid_order.active = false;
        ask_order.active = false;
        risk_status = "KILL_SWITCH_MAX_LOSS";
        return {};
    }
    if (!feed_fresh) {
        bid_order.active = false;
        ask_order.active = false;
        risk_status = "STALE_FEED_QUOTES_CANCELLED";
        return {};
    }
    if (!book.valid_market()) {
        bid_order.active = false;
        ask_order.active = false;
        risk_status = "INVALID_BOOK_QUOTES_CANCELLED";
        return {};
    }

    const paper::MarketState market{
        book.best_bid(), book.best_ask(), true, feed_fresh};
    auto attempt = [&](WorkingOrder& working,
                       paper::RestingSide resting_side,
                       paper::Side ledger_side) -> SimulatedFill {
        if (!working.active) return {};

        paper::RestingOrder model = to_model_order(working, resting_side);
        const paper::FillDelta delta = paper::apply_trade_print(model, trade, 0.001);
        if (!delta) {
            // Commit any queue-ahead depletion even if this print did not fill us.
            store_model_order(working, model);
            return {};
        }

        const paper::OrderIntent fill_intent{ledger_side, delta.price, delta.quantity};
        const paper::RiskDecision decision = risk.check(fill_intent, market, ledger);
        if (risk.halted()) {
            working.active = false;
            bid_order.active = false;
            ask_order.active = false;
            risk_status = "KILL_SWITCH_MAX_LOSS";
            return {};
        }
        if (!decision.allowed) {
            working.active = false;
            risk_status = "TRADE_FILL_REJECTED_" + decision.reason;
            return {};
        }

        store_model_order(working, model);
        ledger.apply_fill(ledger_side, delta.price, delta.quantity);
        risk_status = "TRADE_PRINT_FILL";
        return SimulatedFill{
            ledger_side == paper::Side::Buy ? "BUY" : "SELL",
            delta.price, delta.quantity};
    };

    // Binance aggTrade m=true means buyer was maker, so the aggressor was a seller.
    if (trade.aggressor == paper::AggressorSide::Sell)
        return attempt(bid_order, paper::RestingSide::Buy, paper::Side::Buy);
    return attempt(ask_order, paper::RestingSide::Sell, paper::Side::Sell);
}

'@
$main = $main.Substring(0, $start) + $newModel + $main.Substring($end)
$main = Replace-Once $main 'void run_one_session(paper::Ledger& ledger, paper::RiskManager& risk, std::ofstream& csv) {' 'void run_one_session(paper::Ledger& ledger, paper::RiskManager& risk, std::ofstream& csv, std::ofstream& trade_csv) {' 'session signature'
$main = Replace-Once $main 'std::cout << "Connected to public Binance incremental depth stream; buffering for snapshot sync...\n";' 'std::cout << "Connected to public Binance depth + aggTrade streams; buffering depth for snapshot sync...\n";' 'combined stream startup message'
$main = Replace-Once $main 'std::cout << "SYNCHRONIZED local book is live. Logging to data/integrated_paper_trader.csv.\n"' 'std::cout << "SYNCHRONIZED local book is live. Logging book state to data/integrated_paper_trader.csv and trade events to data/trade_print_fill_events.csv.\n"' 'synchronized log destination message'
$main = Replace-Once $main '            fill = try_simulated_fill(book, working_bid, working_ask, ledger);' '            // No depth-touch fills: a virtual order fills only after a matching public aggTrade print.' 'remove book-touch fills'
$main = Replace-Once $main 'working_bid = WorkingOrder{desired_bid, kOrderQtyBtc, true};' 'working_bid = WorkingOrder{desired_bid, kOrderQtyBtc, true, book.bid_quantity_at_price(desired_bid)};' 'bid queue initialization'
$oldText5 = @'
                            working_ask = WorkingOrder{
                                desired_ask, std::min(kOrderQtyBtc, ledger.inventory()), true};
'@
$newText5 = @'
                            working_ask = WorkingOrder{
                                desired_ask, std::min(kOrderQtyBtc, ledger.inventory()), true,
                                book.ask_quantity_at_price(desired_ask)};
'@
$main = Replace-Once $main $oldText5 $newText5 'ask queue initialization'

# Install the aggTrade handler immediately after the depth-event handler.
$oldText6 = @'
    };

    std::cout << "Snapshot loaded at update ID " << snapshot.last_update_id << ".\n"
'@
$newText6 = @'
    };

    std::size_t trade_print_count = 0;
    std::size_t simulated_fill_count = 0;
    auto process_trade = [&](const ReceivedMessage& received, const json& trade_payload) {
        const paper::TradePrint trade = parse_trade_print(trade_payload);
        const bool feed_fresh = received.local_receive_ms > 0 &&
            system_time_ms() >= received.local_receive_ms &&
            (system_time_ms() - received.local_receive_ms) <= kFeedFreshMs;
        std::string trade_risk_status = "OK";
        const SimulatedFill fill = try_trade_print_fill(
            trade, book, working_bid, working_ask, ledger, risk,
            feed_fresh, trade_risk_status);
        const paper::PortfolioMark mark = ledger.mark_to_market(last_valid_mark_bid);
        const std::int64_t event_time_ms = trade_payload.value(
            "T", trade_payload.value("E", std::int64_t{0}));
        const char* aggressor = trade.aggressor == paper::AggressorSide::Buy ? "BUY" : "SELL";

        trade_csv << received.local_receive_ms << ',' << event_time_ms << ','
                  << std::setprecision(12) << trade.price << ',' << trade.quantity << ','
                  << aggressor << ','
                  << (working_bid.active ? working_bid.price : 0.0) << ','
                  << working_bid.queue_ahead_quantity << ','
                  << (working_ask.active ? working_ask.price : 0.0) << ','
                  << working_ask.queue_ahead_quantity << ','
                  << fill.side << ',' << fill.price << ',' << fill.quantity << ','
                  << mark.cash << ',' << mark.inventory << ',' << mark.net_pnl << ','
                  << mark.realised_net_pnl << ',' << mark.fees_paid << ','
                  << mark.current_drawdown << ',' << std::quoted(trade_risk_status) << '\n';

        ++trade_print_count;
        if (fill.side != "NONE") {
            ++simulated_fill_count;
            std::cout << "[TRADE-PRINT PAPER FILL] " << fill.side << ' '
                      << fill.quantity << " BTC @ " << std::fixed << std::setprecision(2)
                      << fill.price << " | inventory " << std::setprecision(4)
                      << mark.inventory << " BTC | net P&L $" << mark.net_pnl << '\n';
        }
        if (trade_print_count % 100 == 0) trade_csv.flush();
        if (trade_print_count % 1000 == 0) {
            std::cout << "Observed aggTrade prints: " << trade_print_count
                      << " | paper fills: " << simulated_fill_count << '\n';
        }
    };

    std::cout << "Snapshot loaded at update ID " << snapshot.last_update_id << ".\n"
'@
$main = Replace-Once $main $oldText6 $newText6 'aggTrade handler'

# The main live loop must dispatch trade prints instead of discarding non-depth events.
$oldText7 = @'
        if (!is_depth_event(received.payload)) continue;
        const DepthEvent event = parse_event(received.payload);
        const ApplyResult result = book.apply(event);
'@
$newText7 = @'
        if (is_agg_trade_event(received.payload)) {
            process_trade(received, received.payload);
            continue;
        }
        if (!is_depth_event(received.payload)) continue;
        const DepthEvent event = parse_event(received.payload);
        const ApplyResult result = book.apply(event);
'@
$main = Replace-Once $main $oldText7 $newText7 'live-loop aggTrade dispatch'

# Create a separate, append-only event log so the older depth CSV schema stays compatible.
$oldText8 = @'
    if (needs_header) {
        csv << "local_receive_ms,event_time_ms,update_id,best_bid,best_bid_qty,best_ask,best_ask_qty,"
               "spread,mid_price,microprice,depth_imbalance_5,fair_value,desired_bid,working_bid,"
               "desired_ask,working_ask,cash_usd,inventory_btc,net_pnl_usd,realised_net_pnl_usd,"
               "fees_paid_usd,current_drawdown_usd,fill_side,fill_price,fill_qty,risk_status\n";
        csv.flush();
    }

    try {
'@
$newText8 = @'
    if (needs_header) {
        csv << "local_receive_ms,event_time_ms,update_id,best_bid,best_bid_qty,best_ask,best_ask_qty,"
               "spread,mid_price,microprice,depth_imbalance_5,fair_value,desired_bid,working_bid,"
               "desired_ask,working_ask,cash_usd,inventory_btc,net_pnl_usd,realised_net_pnl_usd,"
               "fees_paid_usd,current_drawdown_usd,fill_side,fill_price,fill_qty,risk_status\n";
        csv.flush();
    }

    const std::string trade_csv_path = "data/trade_print_fill_events.csv";
    const bool trade_csv_needs_header = !std::filesystem::exists(trade_csv_path) ||
        std::filesystem::file_size(trade_csv_path) == 0;
    std::ofstream trade_csv(trade_csv_path, std::ios::app);
    if (!trade_csv) {
        std::cerr << "Cannot open " << trade_csv_path << '\n';
        csv.close();
        curl_global_cleanup();
        return 1;
    }
    if (trade_csv_needs_header) {
        trade_csv << "local_receive_ms,event_time_ms,trade_price,trade_qty,aggressor_side,"
                     "working_bid_price,bid_queue_ahead_after_btc,working_ask_price,"
                     "ask_queue_ahead_after_btc,fill_side,fill_price,fill_qty,cash_usd,"
                     "inventory_btc,net_pnl_usd,realised_net_pnl_usd,fees_paid_usd,"
                     "current_drawdown_usd,risk_status\n";
        trade_csv.flush();
    }

    try {
'@
$main = Replace-Once $main $oldText8 $newText8 'trade fill CSV creation'
$main = Replace-Once $main '<< "Quotes target assumed round-trip fees plus an extra edge; fill detection is a simplified book-touch model.\n";' '<< "Quotes target assumed fees plus an extra edge; simulated fills require matching public aggTrade prints.\n";' 'startup fill-model description'
$main = Replace-Once $main 'run_one_session(ledger, risk, csv);' 'run_one_session(ledger, risk, csv, trade_csv);' 'session call'
$oldText9 = @'
                csv.flush();
                std::cerr << "Feed/session reset: " << e.what()
'@
$newText9 = @'
                csv.flush();
                trade_csv.flush();
                std::cerr << "Feed/session reset: " << e.what()
'@
$main = Replace-Once $main $oldText9 $newText9 'flush trade CSV on reconnect'
$oldText10 = @'
        std::cerr << "Fatal initialization error: " << e.what() << '\n';
        csv.close();
        curl_global_cleanup();
        return 1;
'@
$newText10 = @'
        std::cerr << "Fatal initialization error: " << e.what() << '\n';
        csv.close();
        trade_csv.close();
        curl_global_cleanup();
        return 1;
'@
$main = Replace-Once $main $oldText10 $newText10 'close trade CSV on fatal error'
$oldText11 = @'
    csv.flush();
    csv.close();
    curl_global_cleanup();
'@
$newText11 = @'
    csv.flush();
    csv.close();
    trade_csv.flush();
    trade_csv.close();
    curl_global_cleanup();
'@
$main = Replace-Once $main $oldText11 $newText11 'close trade CSV at exit'

# Ignore generated trade-print logs and update both relevant documentation sections.
if (-not $gitignore.Contains('data/trade_print_fill_events.csv')) {
    $gitignore = $gitignore.TrimEnd("`n") + "`n`n# Generated trade-print paper-execution log`ndata/trade_print_fill_events.csv`n"
}
$oldText12 = @'
- Fills are a simplified *book-touch approximation*: a best-bid/best-ask touch triggers a virtual fill. It does not ingest public trade prints, estimate queue position, or reconstruct matching-engine priority. Consequently, simulated fill rate and P&L are not evidence of executable profitability.
'@
$newText12 = @'
- Fills are attributed only when a public Binance `aggTrade` print matches the hypothetical limit price and aggressor direction, after estimated same-price displayed queue volume is consumed. This remains approximate: aggregate public prints do not reveal our actual queue priority, hidden liquidity, or all cancellations. When no same-price level is visible, queue-ahead estimate defaults to zero and may be optimistic. `data/trade_print_fill_events.csv` records the trade-print stream and estimated fills separately from the book-state log. Simulated fill rate and P&L are not evidence of executable profitability.
'@
$readme = Replace-Once $readme $oldText12 $newText12 'integrated README fill model limitation'
$oldText13 = @'
The paper market maker uses simplified book-touch fill detection. Zero fills
means the strategy's profitability has not been demonstrated. No claim of
positive net P&L or live-trading readiness is supported by these results.
'@
$newText13 = @'
The integrated paper market maker now attributes fills from matching public
Binance `aggTrade` prints and an estimated same-price displayed queue ahead.
This is still an approximation: hidden liquidity, exact queue priority and
cancellations cannot be inferred from public aggregate prints. The separate
`data/trade_print_fill_events.csv` log records trade events and estimated fills.
Zero or simulated fills do not establish profitability or live-trading readiness.
'@
$readme = Replace-Once $readme '1. Connect to Binance''s public incremental-depth WebSocket stream.' '1. Connect to Binance''s public combined incremental-depth and aggregate-trade WebSocket streams.' 'integrated README stream description'
$readme = Replace-Once $readme 'Then compile the integrated executable. Treat any compiler error as a blocker; don''t run it until it builds cleanly. I have packaged the source and build recipe, but this new integrated executable has not been compiled in your Windows/MSYS2 environment yet.' 'After applying the trade-print integration, rebuild all targets with `cmake --build build --parallel 2` and run `ctest --test-dir build --output-on-failure`. Verify that the executable connects, writes the trade-print CSV, and reports any simulated fills before interpreting its results. Successful compilation or paper fills do not establish profitability.' 'integrated README final validation instructions'
$generalReadme = Replace-Once $generalReadme $oldText13 $newText13 'general README fill model limitation'
$oldText14 = @'
    assert(near(book.spread(), 1.0));
    assert(near(book.mid_price(), 100.5));
'@
$newText14 = @'
    assert(near(book.spread(), 1.0));
    assert(near(book.mid_price(), 100.5));
    assert(near(book.bid_quantity_at_price(100.0), 2.0));
    assert(near(book.ask_quantity_at_price(101.0), 4.0));
    assert(near(book.bid_quantity_at_price(98.0), 0.0));
'@
$localTest = Replace-Once $localTest $oldText14 $newText14 'local book queue estimate assertions'

# Write only after every replacement and validation succeeded.
Write-Normalized $bookPath $book
Write-Normalized $mainPath $main
Write-Normalized $gitignorePath $gitignore
Write-Normalized $readmePath $readme
Write-Normalized $generalReadmePath $generalReadme
Write-Normalized $localTestPath $localTest
Write-Host 'Integrated Binance aggTrade handling into the paper market maker.'
Write-Host 'Added same-price displayed queue estimates, a separate trade-print event CSV, and local-book queue lookup assertions.'
Write-Host 'Existing paper-trading assumptions and prior CSV schema were preserved.'
Write-Host 'Review git diff before building; no files were committed or pushed.'
