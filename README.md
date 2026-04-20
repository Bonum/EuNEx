# EuNEx — Euronext Optiq Architecture Learning Project

C++ actor-based matching engine that mirrors the Euronext Optiq architecture,
ported from the [StockEx](https://github.com/Bonum/StockEx) Python prototype.

## Architecture Mapping

```
StockEx (Python/Kafka)              EuNEx (C++/Simplx)                 Optiq (Production)
─────────────────────               ──────────────────                  ──────────────────
fix_oeg_server.py        →   OEGatewayActor           →   OEActor
  Kafka 'orders' topic   →     Event::Pipe            →     Simplx Event::Pipe
matcher.py               →   OrderBookActor           →   LogicalCoreActor + Book
  match_order()          →     OrderBook::newOrder()   →     RecoveryCause → IACA Cause → forwardToBook
  handle_cancel()        →     OrderBook::cancelOrder()→     CancelOrderData
  handle_amend()         →     OrderBook::modifyOrder()→     ModifyOrderData
  Kafka 'trades' topic   →     TradeEvent via Pipe     →     IACA fragment chain
dashboard.py (SSE)       →   MarketDataActor           →   MDLimitLogicalCoreHandler
  /orderbook/<sym>       →     BookUpdateEvent         →     PublishLimitUpdateRequest
  /trades                →     TradeEvent              →     IACA → IA SBE message
database.py (SQLite)     →   RecoveryProxy (memory)    →   RecoveryProxy → Kafka
  save_trade()           →     FragmentStore::append()  →   PersistenceAgent → Kafka produce
                                                       →   IATCRecovery SBE envelope
(none)                   →   IacaAggregator            →   IacaAggregatorActor
                              Fragment chains           →     CoherentFragmentChain
                              FragmentHandler           →     FastNewOrderHandler etc.
```

## Actor Topology

```
                    ┌──────────────────────┐
                    │   OEGatewayActor     │  ← External orders in
                    │   (Core 0)           │  ← Execution reports out
                    └──────────┬───────────┘
                               │ NewOrderEvent / CancelOrderEvent
                               ▼
              ┌────────────────────────────────┐
              │      OrderBookActor(s)         │  ← One per symbol
              │      (Core 1)                  │  ← Price-time matching
              └───────┬──────────────┬─────────┘
                      │              │
           ExecReport │              │ TradeEvent + BookUpdateEvent
                      ▼              ▼
    ┌─────────────────┐    ┌─────────────────┐
    │ OEGatewayActor  │    │ MarketDataActor │
    │ (ack back)      │    │ (Core 2)        │
    └─────────────────┘    └─────────────────┘
```

## Build & Run

```bash
# Build (shim mode — no Simplx dependency)
mkdir build && cd build
cmake ..
cmake --build .

# Run matching engine demo
./eunex_me

# Run tests
ctest
# or individually:
./test_orderbook
./test_matching_engine

# Run examples
./ping_pong
./simple_match
```

## With Real Simplx

```bash
git clone https://github.com/Tredzone/simplx.git /path/to/simplx
cmake -DEUNEX_USE_SIMPLX=ON -DSIMPLX_ROOT=/path/to/simplx ..
```

## Project Structure

```
EuNEx/
├── src/
│   ├── main.cpp                    # Entry point — wires actors together
│   ├── engine/
│   │   └── SimplxShim.hpp          # Simplx-compatible API shim
│   ├── common/
│   │   ├── Types.hpp               # Price, Order, Trade, enums
│   │   ├── OrderBook.hpp           # Price-time priority order book
│   │   └── OrderBook.cpp
│   ├── actors/
│   │   ├── Events.hpp              # All inter-actor event types
│   │   ├── OrderBookActor.hpp/cpp  # Matching engine actor
│   │   ├── OEGatewayActor.hpp/cpp  # Order entry gateway actor
│   │   └── MarketDataActor.hpp/cpp # Market data publisher actor
│   ├── recovery/
│   │   └── RecoveryProxy.hpp/cpp   # Recovery Cause/Effect (simplified)
│   └── iaca/
│       ├── Fragment.hpp            # IACA fragment definitions
│       └── IacaAggregator.hpp/cpp  # Fragment chain aggregation
├── examples/
│   ├── ping_pong.cpp               # Actor basics tutorial
│   └── simple_match.cpp            # Matching with Recovery + IACA
├── tests/
│   ├── test_orderbook.cpp          # OrderBook unit tests
│   └── test_matching_engine.cpp    # Actor integration tests
└── CMakeLists.txt
```

## Next Steps

1. **Add real Simplx integration** — replace shim with real multi-threaded actors
2. **Kafka persistence** — replace FragmentStore with actual Kafka produce/consume
3. **SBE encoding** — replace event structs with SBE-encoded messages
4. **FIX gateway** — add FIX 4.4 acceptor (replaces fix_oeg_server.py)
5. **Master/Mirror failover** — implement full Recovery replay on Mirror node
6. **Clearing House actors** — port AI trading members as actors
