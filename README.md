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
# Build (multi-threaded engine, no external dependencies)
mkdir build && cd build
cmake ..
cmake --build . --config Release

# Run matching engine demo
./eunex_me

# Run all tests (26 tests)
ctest
# or individually:
./test_orderbook           # 14 order book unit tests
./test_matching_engine     # 6 actor integration tests
./test_threaded_engine     # 6 multi-threaded engine tests

# Run examples
./ping_pong
./simple_match
```

## With Kafka Persistence

```bash
# Start Kafka (Docker required)
cd docker && docker compose up -d

# Build with Kafka support (Linux — requires librdkafka-dev)
cmake -DEUNEX_USE_KAFKA=ON ..
cmake --build . --config Release
```

## Web Dashboard

```bash
# Install Python dependency
pip install flask

# Run dashboard (opens on http://localhost:8080)
python dashboard/app.py
```

Features: order book view, trade blotter, BBO snapshots, order entry,
session controls, SSE real-time streaming. Supports EQU and EQD segments.

## Docker (Linux Build + Kafka)

```bash
cd docker
docker compose up --build
# Dashboard: http://localhost:8080
# Kafka: localhost:9092
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
│   │   └── SimplxShim.hpp          # Multi-threaded actor engine (Simplx API)
│   ├── common/
│   │   ├── Types.hpp               # Price, Order, Trade, enums
│   │   ├── OrderBook.hpp           # Price-time priority order book
│   │   └── OrderBook.cpp
│   ├── actors/
│   │   ├── Events.hpp              # All inter-actor event types
│   │   ├── OrderBookActor.hpp/cpp  # Matching engine actor
│   │   ├── OEGatewayActor.hpp/cpp  # Order entry gateway actor
│   │   └── MarketDataActor.hpp/cpp # Market data publisher actor
│   ├── persistence/
│   │   ├── PersistenceStore.hpp    # Abstract store + InMemoryStore
│   │   └── KafkaStore.hpp          # Kafka-backed persistence (optional)
│   ├── recovery/
│   │   └── RecoveryProxy.hpp/cpp   # Recovery Cause/Effect (simplified)
│   └── iaca/
│       ├── Fragment.hpp            # IACA fragment definitions
│       └── IacaAggregator.hpp/cpp  # Fragment chain aggregation
├── dashboard/
│   ├── app.py                      # Flask web dashboard (SSE streaming)
│   └── templates/index.html        # Trading UI (order book, trades, BBO)
├── docker/
│   ├── docker-compose.yml          # Kafka (KRaft) + EuNEx container
│   └── Dockerfile                  # Linux multi-stage build
├── docs/
│   └── process-diagram.md          # Architecture diagrams & expansion roadmap
├── examples/
│   ├── ping_pong.cpp               # Actor basics tutorial
│   └── simple_match.cpp            # Matching with Recovery + IACA
├── tests/
│   ├── test_orderbook.cpp          # OrderBook unit tests (14)
│   ├── test_matching_engine.cpp    # Actor integration tests (6)
│   └── test_threaded_engine.cpp    # Multi-threaded engine tests (6)
└── CMakeLists.txt
```

## Next Steps

1. ~~Add real Simplx integration~~ ✓ Multi-threaded actor engine with mailbox queues
2. ~~Kafka persistence~~ ✓ KafkaStore + Docker Compose (KRaft mode)
3. **SBE encoding** — replace event structs with SBE-encoded messages
4. **FIX gateway** — add FIX 4.4 acceptor (replaces fix_oeg_server.py)
5. **Master/Mirror failover** — implement full Recovery replay on Mirror node
6. **Clearing House actors** — port AI trading members as actors
