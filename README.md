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
database.py (SQLite)     →   SQLite persistence        →   RecoveryProxy → Kafka
  save_trade()           →     dashboard/database.py   →   PersistenceAgent → Kafka produce
fix_oeg_server.py        →   fix_gateway/fix_server.py →   FIX 4.4 OEG Acceptor
  NewOrderSingle         →     35=D handling           →     Optiq FIX gateway
  ExecutionReport        →     35=8 response           →     Execution reports
ch_ai_trader.py          →   clearing_house/           →   Clearing House members
  AI strategies          →     momentum/mean_revert    →     Trading obligations
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
│ SSE Streaming   │ │ SQLite DB       │ │                  │
└────────┬────────┘ └��──────┬──────────┘ └────────┬────────┘
         │                  │                      │
         │    HTTP REST     │    HTTP REST          │
         ◄──────────────────┘◄─────────────────────┘
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
mkdir build && cd build
cmake ..
cmake --build . --config Release

# Run matching engine demo
./eunex_me

# Run all tests (26 tests)
ctest
```

## With Kafka Persistence

```bash
cmake -DEUNEX_USE_KAFKA=ON ..
cmake --build . --config Release
```

## FIX Gateway

```bash
# Start FIX server (requires dashboard running)
python fix_gateway/fix_server.py

# Test with built-in client
python fix_gateway/fix_server.py test
```

Supports: NewOrderSingle (35=D), OrderCancelRequest (35=F),
OrderCancelReplaceRequest (35=G), ExecutionReport (35=8).

## Clearing House

```bash
# Start clearing house (requires dashboard running)
python clearing_house/app.py
```

Features:
- 10 AI trading members (MBR01-MBR10) with 3 strategies
- Strategies: momentum, mean_reversion, random
- Member portal with login, portfolio, order submission
- Leaderboard with capital, holdings, P&L
- Daily trading obligation (min 20 securities)
- EOD settlement

## Web Dashboard

Features:
- Real-time order book with bid/ask depth bars
- OHLCV price charts (1H, 8H, 1D, 1W) via Chart.js
- Order entry with Limit/Market, TIF (Day/GTC/IOC/FOK)
- Order amend and cancel
- Trade blotter with history
- Market snapshots (BBO, last price, volume)
- Clearing House leaderboard integration
- Session controls (Start/Suspend/Resume/End Day)
- SSE real-time streaming
- SQLite persistence for orders, trades, OHLCV
- EQU (Cash/Equities) and EQD (Derivatives) segments

## Project Structure

```
EuNEx/
├── src/                                # C++ matching engine
│   ├── main.cpp                        # Entry point
│   ├── engine/SimplxShim.hpp           # Multi-threaded actor engine
│   ├── common/
│   │   ├── Types.hpp                   # Price, Order, Trade, enums
│   │   ├── OrderBook.hpp/cpp           # Price-time priority matching
│   ├── actors/
│   │   ├── Events.hpp                  # Inter-actor event types
│   │   ├── OrderBookActor.hpp/cpp      # Matching engine actor
│   │   ├── OEGatewayActor.hpp/cpp      # Order entry gateway
│   │   └── MarketDataActor.hpp/cpp     # Market data publisher
│   ├── persistence/
│   │   ├── PersistenceStore.hpp        # Abstract store + InMemoryStore
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
│   └── fix_server.py                   # FIX 4.4 TCP acceptor
├── clearing_house/
│   ├── app.py                          # Flask CH portal + API
│   ├── ch_database.py                  # SQLite (members, holdings, settlements)
│   ├── ch_ai_trader.py                 # AI trading strategies
│   └── templates/                      # CH web UI (login, dashboard, portfolio)
├── shared/
│   └── config.py                       # Centralized configuration
├── docker/
│   ├── docker-compose.yml              # Kafka + EuNEx (all services)
���   ├── Dockerfile                      # Multi-stage Linux build
│   └── nginx.conf                      # Reverse proxy configuration
├── run.sh                              # Linux startup script
├── tests/
│   ├── test_orderbook.cpp              # OrderBook unit tests (14)
│   ├── test_matching_engine.cpp        # Actor integration tests (6)
│   └── test_threaded_engine.cpp        # Multi-threaded engine tests (6)
├── examples/
│   ├── ping_pong.cpp                   # Actor basics tutorial
│   └── simple_match.cpp                # Matching with Recovery + IACA
└── docs/process-diagram.md             # Architecture diagrams
```

## Next Steps

1. ~~Add real Simplx integration~~ ✓ Multi-threaded actor engine with mailbox queues
2. ~~Kafka persistence~~ ✓ KafkaStore + Docker Compose (KRaft mode)
3. **SBE encoding** — replace event structs with SBE-encoded messages
4. ~~FIX gateway~~ ✓ Pure Python FIX 4.4 TCP acceptor
5. **Master/Mirror failover** — implement full Recovery replay on Mirror node
6. ~~Clearing House actors~~ ✓ AI trading members with 3 strategies + portal
