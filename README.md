# EuNEx — Euronext Optiq Architecture Learning Project

C++ actor-based matching engine that mirrors the Euronext Optiq architecture,
ported from the [StockEx](https://github.com/Bonum/StockEx) Python prototype.

## Architecture Mapping

```
StockEx (Python/Kafka)              EuNEx (C++/Simplx)                 Optiq (Production)
─────────────────────               ──────────────────                  ──────────────────
fix_oeg_server.py        →   OEGActor                 →   OEActor
  Kafka 'orders' topic   →     Event::Pipe            →     Simplx Event::Pipe
matcher.py               →   MECoreActor              →   LogicalCoreActor + Book
  match_order()          →     Book::newOrder()        →     RecoveryCause → IACA Cause → forwardToBook
  handle_cancel()        →     Book::cancelOrder()     →     CancelOrderData
  handle_amend()         →     Book::modifyOrder()     →     ModifyOrderData
  Kafka 'trades' topic   →     TradeEvent via Pipe     →     IACA fragment chain
dashboard.py (SSE)       →   MDGActor                  →   MDLimitLogicalCoreHandler
  /orderbook/<sym>       →     BookUpdateEvent         →     PublishLimitUpdateRequest
  /trades                →     TradeEvent              →     IACA → IA SBE message
database.py (SQLite)     →   RecoveryProxy (memory)    →   RecoveryProxy → Kafka
  save_trade()           →     FragmentStore::append() →   PersistenceAgent → Kafka produce
fix_oeg_server.py        →   FIXAcceptorActor          →   FIX 4.4 OEG Acceptor
  NewOrderSingle         →     35=D handling           →     Optiq FIX gateway
  ExecutionReport        →     35=8 response           →     Execution reports
ch_ai_trader.py          →   ClearingHouseActor        →   Clearing House (PTB path)
  AI strategies          →   AITraderActor             →   Trading obligations
```

## Actor Topology (v0.6)

```
 Core 0: OEGActor + FIXAcceptorActor     ← Order entry & FIX protocol
 Core 1: MECoreActor (per symbol)         ← Matching engine (Book)
 Core 2: MDGActor                         ← Market data snapshots
 Core 3: ClearingHouseActor + AITrader    ← Post-trade & AI members
```

## Service Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    nginx (:7860)                        │
│              Reverse proxy (Docker)                      │
└────────┬───────────────────┬───────────────────┬────────┘
         │                   │                   │
         ▼                   ▼                   ▼
┌─────────────────┐ ┌──────────────────┐ ┌──────────────────┐
│   Dashboard     │ │ Clearing House   │ │  FIX Gateway     │
│   (:8090)       │ │ (:8091)          │ │  (:9001 TCP)     │
│                 │ │                  │ │                  │
│ Order Book      │ │ 10 AI Members   │ │ FIX 4.4 Acceptor│
│ Trade Charts    │ │ Leaderboard     │ │ NewOrder/Cancel  │
│ OHLCV History   │ │ Portfolios      │ │ Amend/ExecRpt   │
│ SQLite DB       │ │ Settlements     │ │                  │
│ SSE Streaming   │ │                 │ │                  │
└────────┬────────┘ └────────┬────────┘ └────────┬────────┘
         │                   │                    │
         └───────────────────┴────────────────────┘
                             │
                             ▼
               ┌─────────────────────────────┐
               │    C++ Matching Engine      │
               │    (eunex_me)               │
               │    Multi-threaded actors    │
               │    Price-time priority      │
               └─────────────────────────────┘
```

## Quick Start (Linux)

```bash
# Install dependency
pip install flask

# Start all services (dashboard + FIX gateway + clearing house)
./run.sh

# Services:
#   Dashboard:       http://localhost:8090
#   Clearing House:  http://localhost:8091
#   FIX Gateway:     localhost:9001 (TCP)

# Stop all
./run.sh stop

# Check status
./run.sh status
```

## Quick Start (Docker — Windows/Linux)

```bash
cd docker
docker compose up --build

# All services via nginx:
#   Dashboard:       http://localhost:7860
#   Clearing House:  http://localhost:7860/ch/
#   Kafka:           localhost:9092
```

## Build C++ Engine

```bash
cmake -B build -DEUNEX_BUILD_TESTS=ON
cmake --build build --config Release

# Run matching engine
./build/Release/eunex_me

# Run all tests (7 suites)
cd build && ctest -C Release
```

## With Kafka Persistence

```bash
# Compile with Kafka support (requires librdkafka-dev)
cmake -B build -DEUNEX_USE_KAFKA=ON
cmake --build build --config Release

# Run with Kafka (set broker address)
EUNEX_KAFKA_BROKERS=localhost:9092 ./build/Release/eunex_me

# Topics: eunex.orders, eunex.trades, eunex.market-data, eunex.recovery.fragments
```

Without `EUNEX_USE_KAFKA`, the engine compiles with a no-op stub and runs standalone.

## FIX Gateway

The C++ engine includes a built-in FIX 4.4 acceptor on TCP port 9001.
A Python FIX gateway is also available:

```bash
python fix_gateway/fix_server.py
python fix_gateway/fix_server.py test
```

Supports: NewOrderSingle (35=D), OrderCancelRequest (35=F),
OrderCancelReplaceRequest (35=G), ExecutionReport (35=8).

## Clearing House

10 AI trading members (MBR01-MBR10) with 3 strategies:
- **Momentum**: follow price trends
- **Mean Reversion**: fade price moves
- **Random**: noise trading

Features: capital tracking, holdings per symbol, P&L, leaderboard.

## Project Structure

```
EuNEx/
├── src/                                # C++ matching engine
│   ├── main.cpp                        # Entry point, actor wiring
│   ├── engine/SimplxShim.hpp           # Multi-threaded actor engine
│   ├── common/
│   │   ├── Types.hpp                   # Price, Order, Trade, enums
│   │   └── Book.hpp/cpp               # Price-time priority matching
│   ├── actors/
│   │   ├── Events.hpp                  # Inter-actor event types
│   │   ├── OEGActor.hpp/cpp           # Order Entry Gateway
│   │   ├── MECoreActor.hpp/cpp        # Matching Engine core (per symbol)
│   │   ├── MDGActor.hpp/cpp           # Market Data Gateway
│   │   ├── FIXAcceptorActor.hpp/cpp   # FIX 4.4 TCP acceptor
│   │   ├── ClearingHouseActor.hpp/cpp # Trade clearing & member positions
│   │   └── AITraderActor.hpp/cpp      # Automated trading members
│   ├── net/SocketCompat.hpp           # Cross-platform socket abstraction
│   ├── persistence/
│   │   ├── PersistenceStore.hpp        # Abstract store + InMemoryStore
│   │   ├── KafkaBus.hpp                # Multi-topic Kafka publisher (Optiq KFK)
│   │   └── KafkaStore.hpp              # Kafka persistence (optional)
│   ├── recovery/RecoveryProxy.hpp/cpp  # Recovery Cause/Effect
│   └── iaca/
│       ├── Fragment.hpp                # IACA fragment definitions
│       └── IacaAggregator.hpp/cpp      # Fragment chain aggregation
├── dashboard/
│   ├── app.py                          # Flask dashboard + matching engine
│   ├── database.py                     # SQLite (orders, trades, OHLCV)
│   └── templates/index.html            # Trading UI with Chart.js
├── fix_gateway/
│   └── fix_server.py                   # Python FIX 4.4 TCP acceptor
├── clearing_house/
│   ├── app.py                          # Flask CH portal + API
│   ├── ch_database.py                  # SQLite (members, holdings)
│   ├── ch_ai_trader.py                 # AI trading strategies
│   └── templates/                      # CH web UI
├── shared/config.py                    # Centralized configuration
├── docker/
│   ├── docker-compose.yml              # Kafka + EuNEx (all services)
│   ├── Dockerfile                      # Multi-stage Linux build
│   └── nginx.conf                      # Reverse proxy configuration
├── tests/
│   ├── test_orderbook.cpp              # Book unit tests (26 cases)
│   ├── test_matching_engine.cpp        # ME integration tests
│   ├── test_threaded_engine.cpp        # Multi-threaded engine tests
│   ├── test_clearing_house.cpp         # Clearing house tests (7 cases)
│   ├── test_fix_gateway.cpp            # FIX gateway tests (5 cases)
│   └── test_ai_trader.cpp             # AI trader tests (6 cases)
├── examples/
│   ├── ping_pong.cpp                   # Actor basics tutorial
│   └── simple_match.cpp                # Matching with Recovery + IACA
└── docs/
    ├── developers-guide.md             # Detailed developers guide
    └── process-diagram.md              # Architecture diagrams
```

## Documentation

- **[Developers Guide](docs/developers-guide.md)** — Detailed architecture, data flow, component reference
- **[Process Diagram](docs/process-diagram.md)** — Optiq architecture diagrams and roadmap

## Next Steps

1. ~~Multi-threaded actor engine~~ ✓ SimplxShim with mailbox queues
2. ~~Kafka persistence~~ ✓ KafkaStore + Docker Compose (KRaft mode)
3. ~~FIX gateway~~ ✓ C++ FIXAcceptorActor + Python fallback
4. ~~Clearing House~~ ✓ ClearingHouseActor + AITraderActor
5. **SBE encoding** — replace event structs with SBE-encoded messages
6. **Master/Mirror failover** — implement full Recovery replay on Mirror node
7. **Trading phases** — pre-open, uncrossing, continuous, close, TAL
8. **Additional order types** — Stop, Pegged, Mid-Point, Iceberg
