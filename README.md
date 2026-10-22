

\# C++ Quantitative Trading \& Market-Making Simulator



An educational C++20 project exploring limit order books, market

microstructure, order-book imbalance, inventory-aware quoting,

simulated trade execution, and trading risk analytics.



\## Project status



Version 1 — core educational components implemented.



\## Features



\- Limit order book with price-time-priority matching

\- Partial fills and order cancellation

\- Best bid and ask queries

\- Order-book imbalance and microprice calculations

\- Directional signal backtesting

\- Simplified entry/exit P\&L estimates

\- Inventory-aware market-making quote generation

\- Simulated order fills and inventory tracking

\- Cash ledger and mark-to-market P\&L

\- Risk controls and edge-case tests

\- Quote calculation microbenchmark



\## Technology



\- C++20

\- GCC / MinGW-w64

\- Windows Command Prompt

\- CMake project configuration

\- Standard C++ library

\- CSV-based educational market-data input



\## Project structure



```text

quant-lob/

├── CMakeLists.txt

├── README.md

├── data/

│   └── sample\_market\_data.csv

├── src/

│   ├── main.cpp

│   ├── order\_book.cpp

│   ├── order\_book.hpp

│   ├── strategy\_demo.cpp

│   ├── imbalance\_backtest.cpp

│   ├── signal\_pnl\_backtest.cpp

│   ├── market\_maker\_demo.cpp

│   ├── market\_maker\_fill\_sim.cpp

│   ├── market\_maker\_ledger.cpp

│   ├── market\_maker\_risk\_report.cpp

│   ├── risk\_controls\_demo.cpp

│   └── strategy\_validation.cpp

├── tests/

│   └── test\_order\_book.cpp

└── build/

&#x20;   └── Generated executables (not source-controlled)

```



\## Build the order-book demonstration



From the project root on Windows:



```bat

if not exist build mkdir build

g++ -std=c++20 -O2 -Wall -Wextra -Isrc src\\order\_book.cpp src\\main.cpp -o build\\lob\_demo.exe

build\\lob\_demo.exe

```



\## Run the order-book unit tests



```bat

g++ -std=c++20 -Wall -Wextra -Isrc src\\order\_book.cpp tests\\test\_order\_book.cpp -o build\\test\_order\_book.exe

build\\test\_order\_book.exe

```



\## Build and run the quantitative examples



Compile each example from the project root.



```bat

g++ -std=c++20 -O2 -Wall -Wextra -Isrc src\\order\_book.cpp src\\strategy\_demo.cpp -o build\\strategy\_demo.exe

build\\strategy\_demo.exe



g++ -std=c++20 -O2 -Wall -Wextra src\\imbalance\_backtest.cpp -o build\\imbalance\_backtest.exe

build\\imbalance\_backtest.exe



g++ -std=c++20 -O2 -Wall -Wextra src\\signal\_pnl\_backtest.cpp -o build\\signal\_pnl\_backtest.exe

build\\signal\_pnl\_backtest.exe



g++ -std=c++20 -O2 -Wall -Wextra src\\market\_maker\_demo.cpp -o build\\market\_maker\_demo.exe

build\\market\_maker\_demo.exe



g++ -std=c++20 -O2 -Wall -Wextra src\\market\_maker\_fill\_sim.cpp -o build\\market\_maker\_fill\_sim.exe

build\\market\_maker\_fill\_sim.exe



g++ -std=c++20 -O2 -Wall -Wextra src\\market\_maker\_ledger.cpp -o build\\market\_maker\_ledger.exe

build\\market\_maker\_ledger.exe



g++ -std=c++20 -O2 -Wall -Wextra src\\market\_maker\_risk\_report.cpp -o build\\market\_maker\_risk\_report.exe

build\\market\_maker\_risk\_report.exe



g++ -std=c++20 -O2 -Wall -Wextra src\\risk\_controls\_demo.cpp -o build\\risk\_controls\_demo.exe

build\\risk\_controls\_demo.exe



g++ -std=c++20 -O2 -Wall -Wextra src\\strategy\_validation.cpp -o build\\strategy\_validation.exe

build\\strategy\_validation.exe

```



\## Important limitations



\- The sample market data and simulated order flow are synthetic.

\- Fill priority, latency, slippage, market impact, and execution are simplified.

\- Reported P\&L is not evidence of a profitable trading strategy.

\- The microbenchmark measures selected quote calculations, not complete trading-system latency.

\- Real-data testing and a realistic execution model remain necessary before drawing conclusions about profitability.



\## Future improvements



\- Replay real historical order-book data

\- Improve exchange execution and queue-position modeling

\- Add transaction-cost and market-impact models

\- Test strategies on separate training and evaluation periods

\- Benchmark the complete pipeline

\- Improve automated tests and integrate them with CMake/CTest



\## Disclaimer



This project is for educational and research purposes. It is not

connected to an exchange and does not place live orders or provide

financial advice.




## Validation snapshot

### Tests
- Order-book unit tests: passed.
- Local synchronized order-book tests: passed.
- P&L and risk engine: all 22 tests passed.

### Live-data smoke test
- Connected to the public Binance BTC/USDT market-data stream.
- Loaded a REST snapshot and synchronized the local order book.
- Continued receiving market updates during the observed run.
- No simulated fills were recorded in the examined CSV history.

### Exploratory imbalance analysis
Using a threshold of +/-0.2 and a forward horizon of 10 recorded rows:
- Positive imbalance: 446 observations, 82 upward moves, 7 downward moves,
  357 unchanged observations, mean forward move +$0.4726 per BTC.
- Negative imbalance: 317 observations, 50 downward moves, 8 upward moves,
  259 unchanged observations, mean forward move -$0.8490 per BTC.
- 20 timestamp gaps were skipped by the exploratory analysis.

These statistics are exploratory, not a validated trading backtest. The CSV
contains repeated runs, adjacent observations overlap, and the analysis does
not model actual fills, queue position, slippage, or trading costs.

### Limitations
The paper market maker uses simplified book-touch fill detection. Zero fills
means the strategy's profitability has not been demonstrated. No claim of
positive net P&L or live-trading readiness is supported by these results.
