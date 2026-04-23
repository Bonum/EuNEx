# EuNEx Developers Guide

**Version 0.5.0** | Euronext Optiq-Modeled Exchange Simulator

---

## Table of Contents

1. [Overview](#1-overview)
2. [Architecture](#2-architecture)
3. [Optiq Naming Alignment](#3-optiq-naming-alignment)
4. [Actor Topology](#4-actor-topology)
5. [Data Flow](#5-data-flow)
6. [Core Components](#6-core-components)
7. [Event System](#7-event-system)
8. [Order Book & Matching](#8-order-book--matching)
9. [Recovery & IACA](#9-recovery--iaca)
10. [Kafka Bus](#10-kafka-bus)
11. [FIX Protocol Gateway](#11-fix-protocol-gateway)
12. [Clearing House](#12-clearing-house)
13. [AI Trading Members](#13-ai-trading-members)
14. [Project Structure](#14-project-structure)
15. [Build & Test](#15-build--test)
16. [Configuration](#16-configuration)
17. [Extending EuNEx](#17-extending-eunex)

---

## 1. Overview

EuNEx (Euronext Exchange Simulator) is a C++20 actor-based matching engine that mirrors the Euronext Optiq production architecture. It implements the full order lifecycle: entry, validation, matching, market data dissemination, trade clearing, and FIX protocol connectivity.

```
  ┌─────────────────────────────────────────────────────────────────────┐
  │                         EuNEx v0.4.0                                │
  │                                                                     │
  │   FIX 4.4 Clients ──► OEG ──► ME (per symbol) ──► MDG             │
  │                         │                    │                       │
  │                         ◄── Exec Reports ◄───┘                      │
  │                                              │                      │
  │                                         Clearing House              │
  │                                              │                      │
  │                                         AI Traders (x10)            │
  └─────────────────────────────────────────────────────────────────────┘
```

**Key design principles:**
- Actor-per-core isolation (no shared mutable state in the hot path)
- Lock-free `Event::Pipe` for cross-actor communication
- Fixed-point pricing (8 decimal places, `PRICE_SCALE = 10^8`)
- Price-time priority matching with multi-level sweep
- Master/Mirror recovery gating for high availability

---

## 2. Architecture

### High-Level System Diagram

```
 ┌─────────────────────────────────────────────────────────────────────────────┐
 │                           EXTERNAL CLIENTS                                  │
 │                      FIX 4.4 / Direct API / Python Bridge                   │
 └─────────────────────────┬───────────────────────────────┬───────────────────┘
                           │                               │
                    TCP :9001                        Direct API
                           │                               │
 ╔═════════════════════════╪═══════════════════════════════╪═══════════════════╗
 ║  CORE 0 — Gateway                                                          ║
 ║  ┌──────────────────────┴───┐    ┌──────────────────────┴───┐              ║
 ║  │   FIXAcceptorActor       │    │     OEGActor             │              ║
 ║  │                          │───►│                          │              ║
 ║  │  • TCP accept loop       │    │  • Session validation    │              ║
 ║  │  • FIX 4.4 parse/build   │    │  • Symbol routing        │              ║
 ║  │  • Logon/Logout/HB       │    │  • Exec report fanout    │              ║
 ║  │  • D, F, G → Events      │    │  • Cancel, Modify        │              ║
 ║  └──────────────────────────┘    └──────────┬───────────────┘              ║
 ╚═════════════════════════════════════════════╪═══════════════════════════════╝
                                               │ NewOrderEvent
                                               │ CancelOrderEvent
                                               │ ModifyOrderEvent
                                               │ (Event::Pipe)
                                               ▼
 ╔═════════════════════════════════════════════════════════════════════════════╗
 ║  CORE 1 — Matching Engine (one MECoreActor per symbol)                     ║
 ║                                                                             ║
 ║  ┌───────────────┐ ┌───────────────┐ ┌───────────────┐ ┌───────────────┐   ║
 ║  │ MECoreActor   │ │ MECoreActor   │ │ MECoreActor   │ │ MECoreActor   │   ║
 ║  │ AAPL (sym=1)  │ │ MSFT (sym=2)  │ │ GOOGL (sym=3) │ │ EURO50 (sym=4)│   ║
 ║  │               │ │               │ │               │ │               │   ║
 ║  │  ┌─────────┐  │ │  ┌─────────┐  │ │  ┌─────────┐  │ │  ┌─────────┐  │   ║
 ║  │  │  Book   │  │ │  │  Book   │  │ │  │  Book   │  │ │  │  Book   │  │   ║
 ║  │  │ (bids)  │  │ │  │ (bids)  │  │ │  │ (bids)  │  │ │  │ (bids)  │  │   ║
 ║  │  │ (asks)  │  │ │  │ (asks)  │  │ │  │ (asks)  │  │ │  │ (asks)  │  │   ║
 ║  │  └─────────┘  │ │  └─────────┘  │ │  └─────────┘  │ │  └─────────┘  │   ║
 ║  └───────┬───────┘ └───────┬───────┘ └───────┬───────┘ └───────┬───────┘   ║
 ║          │                 │                 │                 │             ║
 ║          └────────┬────────┴────────┬────────┴────────┬────────┘             ║
 ╚═══════════════════╪════════════════╪════════════════╪═══════════════════════╝
                     │                │                │
            ExecReport│       TradeEvent│      BookUpdate│
            (→ OEG)   │       (→ MDG)   │      (→ MDG)   │
                     │                │                │
 ╔═══════════════════╪════════════════╪════════════════╪═══════════════════════╗
 ║  CORE 2 — Market Data             ▼                ▼                        ║
 ║  ┌─────────────────────────────────────────────────────────────────────┐     ║
 ║  │  MDGActor                                                           │     ║
 ║  │                                                                     │     ║
 ║  │  • BBO snapshots per symbol (bestBid, bestAsk, depths)             │     ║
 ║  │  • Recent trade history (last 200 trades)                          │     ║
 ║  │  • Trade count, volume, last price per symbol                      │     ║
 ║  │  • Thread-safe reads for Python dashboard bridge                   │     ║
 ║  └─────────────────────────────────────────────────────────────────────┘     ║
 ╚═════════════════════════════════════════════════════════════════════════════╝
                                               │
                                          TradeEvent
                                          (→ Clearing)
                                               │
 ╔═════════════════════════════════════════════╪═══════════════════════════════╗
 ║  CORE 3 — Post-Trade & AI                  ▼                                ║
 ║  ┌─────────────────────────────┐    ┌─────────────────────────────┐         ║
 ║  │  ClearingHouseActor         │    │  AITraderActor              │         ║
 ║  │                             │    │                             │         ║
 ║  │  • Session → Member map     │    │  • 10 members (MBR01-10)   │         ║
 ║  │  • Capital tracking         │    │  • 3 strategies:           │         ║
 ║  │  • Holdings per symbol      │    │    momentum, mean_revert,  │         ║
 ║  │  • P&L calculation          │    │    random                  │         ║
 ║  │  • Leaderboard (sorted)     │    │  • Periodic order bursts   │         ║
 ║  │  • Thread-safe for bridge   │    │  • BBO + price history     │         ║
 ║  └─────────────────────────────┘    └──────────────┬──────────────┘         ║
 ║                                                     │                       ║
 ║                                              NewOrderEvent                  ║
 ║                                              (→ OEG → ME)                   ║
 ╚═════════════════════════════════════════════════════════════════════════════╝
```

---

## 3. Optiq Naming Alignment

EuNEx components are named to match Euronext Optiq production terminology:

```
 Optiq Production                EuNEx Class             File
 ═══════════════════════         ════════════════════     ═══════════════════════════
 OEActor (OE Gateway)      ───► OEGActor                 src/actors/OEGActor.hpp
 LogicalCoreActor + Book   ───► MECoreActor              src/actors/MECoreActor.hpp
 Book (order book engine)  ───► Book                     src/common/Book.hpp
 MDLimitLogicalCoreHandler ───► MDGActor                 src/actors/MDGActor.hpp
 OEG FIX Gateway           ───► FIXAcceptorActor         src/actors/FIXAcceptorActor.hpp
 Clearing House (PTB path) ───► ClearingHouseActor       src/actors/ClearingHouseActor.hpp
 Member Trading Bots       ───► AITraderActor            src/actors/AITraderActor.hpp
 RecoveryProxy             ───► RecoveryProxy            src/recovery/RecoveryProxy.hpp
 IacaAggregatorActor       ───► IacaAggregator           src/iaca/IacaAggregator.hpp
```

**Why these names?**
- **OEG** = Order Entry Gateway (the Optiq front-end for all order flow)
- **ME** = Matching Engine (the core `Book` + `LogicalCoreActor` in Optiq)
- **MDG** = Market Data Gateway (publishes BBO, trades, depth to consumers)
- **FIXAcceptor** = Optiq's FIX protocol acceptor component within OEG

---

## 4. Actor Topology

### Core Affinity Layout

```
 ┌──────────────────────────────────────────────────────────────────────────────┐
 │                                                                              │
 │  ┌─ CPU Core 0 ─────────────────┐    ┌─ CPU Core 1 ────────────────────┐   │
 │  │                               │    │                                 │   │
 │  │  OEGActor                     │    │  MECoreActor (AAPL, sym=1)     │   │
 │  │   • Symbol → Book routing     │    │  MECoreActor (MSFT, sym=2)     │   │
 │  │   • Exec report fanout        │    │  MECoreActor (GOOGL, sym=3)    │   │
 │  │                               │    │  MECoreActor (EURO50, sym=4)   │   │
 │  │  FIXAcceptorActor             │    │                                 │   │
 │  │   • TCP :9001                 │    │  Each owns a Book instance     │   │
 │  │   • FIX 4.4 protocol         │    │  Single-threaded per actor     │   │
 │  │                               │    │  No locks in matching path     │   │
 │  └───────────────────────────────┘    └─────────────────────────────────┘   │
 │                                                                              │
 │  ┌─ CPU Core 2 ─────────────────┐    ┌─ CPU Core 3 ────────────────────┐   │
 │  │                               │    │                                 │   │
 │  │  MDGActor                     │    │  ClearingHouseActor            │   │
 │  │   • BBO per symbol            │    │   • 10 members, capital, P&L   │   │
 │  │   • Trade history             │    │   • Session → Member mapping   │   │
 │  │   • Snapshot queries          │    │                                 │   │
 │  │                               │    │  AITraderActor                  │   │
 │  │                               │    │   • 10 AI members              │   │
 │  │                               │    │   • Momentum / MeanRev / Rand  │   │
 │  └───────────────────────────────┘    └─────────────────────────────────┘   │
 │                                                                              │
 └──────────────────────────────────────────────────────────────────────────────┘
```

### Event::Pipe Connections

```
 FIXAcceptorActor ──NewOrderEvent──────► OEGActor ──NewOrderEvent──► MECoreActor
                                                  ──CancelOrderEvent──►
                                                  ──ModifyOrderEvent──►

 MECoreActor ──ExecReportEvent──► OEGActor ──ExecReportEvent──► FIXAcceptorActor
                                           ──ExecReportEvent──► AITraderActor

 MECoreActor ──TradeEvent──────► MDGActor
             ──BookUpdateEvent─► MDGActor
             ──TradeEvent──────► ClearingHouseActor

 AITraderActor ──NewOrderEvent──► OEGActor (via pipe)
```

---

## 5. Data Flow

### Order Lifecycle (New Limit Order)

```
 ┌──────────┐    FIX 35=D     ┌──────────────────┐   NewOrderEvent    ┌──────────────┐
 │  Client  │ ──────────────► │ FIXAcceptorActor  │ ────────────────► │   OEGActor   │
 │ (FIX)    │                 │ parse FIX tags    │                   │ route by sym │
 └──────────┘                 │ map symbol string │                   └──────┬───────┘
                              │ to SymbolIndex_t  │                          │
                              └───────────────────┘                          │
                                                                    NewOrderEvent
                                                                    (Event::Pipe)
                                                                             │
                                                                             ▼
                              ┌────────────────────────────────────────────────────┐
                              │                  MECoreActor                        │
                              │                                                    │
                              │   1. RecoveryProxy.cause() — persist fragment      │
                              │   2. Book.newOrder() — price-time matching         │
                              │   3. For each fill:                                │
                              │      a. Trade generated (buyer + seller session)   │
                              │      b. TradeEvent → MDGActor                     │
                              │      c. TradeEvent → ClearingHouseActor           │
                              │   4. ExecReportEvent → OEGActor (ack/fill/reject) │
                              │   5. BookUpdateEvent → MDGActor (new BBO)         │
                              │   6. IACA fragments → IacaAggregator              │
                              └────────────────────────────────────────────────────┘
                                         │                          │
                                ExecReportEvent              TradeEvent
                                         │                          │
                                         ▼                          ▼
                              ┌───────────────────┐    ┌──────────────────────┐
                              │    OEGActor        │    │  ClearingHouseActor  │
                              │ fanout to:         │    │  • map session→member│
                              │  • FIXAcceptor     │    │  • update capital    │
                              │  • AITrader        │    │  • update holdings   │
                              └────────┬──────────┘    └──────────────────────┘
                                       │
                              FIX 35=8 (ExecReport)
                                       │
                                       ▼
                              ┌───────────────────┐
                              │     Client         │
                              │  receives fill     │
                              └───────────────────┘
```

### Market Data Flow

```
 MECoreActor ──TradeEvent──────────────► MDGActor
             ──BookUpdateEvent──────────►    │
                                              │
                                              ▼
                                     ┌──────────────────────┐
                                     │  MarketDataSnapshot   │
                                     │  per SymbolIndex_t:   │
                                     │                       │
                                     │  • bestBid, bestAsk  │
                                     │  • lastTradePrice    │
                                     │  • lastTradeQty      │
                                     │  • totalBidQty       │
                                     │  • totalAskQty       │
                                     │  • tradeCount        │
                                     │  • updateTime (ns)   │
                                     │                       │
                                     │  recentTrades[]      │
                                     │  (last 200 trades)   │
                                     └──────────────────────┘
                                              │
                                     getSnapshot() / getRecentTrades()
                                     (thread-safe, read by Python bridge)
```

---

## 6. Core Components

### 6.1 SimplxShim — Actor Framework (`src/engine/SimplxShim.hpp`)

The actor engine emulates the [Tredzone Simplx](https://github.com/Tredzone/simplx) framework API:

| Simplx Concept        | EuNEx Implementation                          |
|----------------------|-----------------------------------------------|
| `Actor`              | Base class with event handlers, mailbox        |
| `Event::Pipe`        | Lock-free cross-actor channel                  |
| `ActorId`            | `{id: uint64, coreId: uint8}`                 |
| `Engine`             | Multi-threaded scheduler with core affinity    |
| `Callback`           | Timer-like periodic invocation                 |
| `AsyncService`       | Service locator pattern                        |

**Two operating modes:**
- **Synchronous** (tests): events delivered inline, single thread
- **Threaded** (`Engine::runFor`): per-core OS threads, mailbox queues

```
 Synchronous Mode (unit tests)          Threaded Mode (Engine::runFor)
 ═══════════════════════════            ═══════════════════════════════

 ActorA.pipe.push<Event>()              ActorA.pipe.push<Event>()
       │                                       │
       ▼                                       ▼
 ActorB.onEvent() ← inline              Mailbox[coreB].enqueue(λ)
                                               │
                                        Core B thread:
                                         mbox.waitAndDrain()
                                               │
                                               ▼
                                        ActorB.onEvent()
```

### 6.2 Types (`src/common/Types.hpp`)

```cpp
Price_t       = int64_t     // Fixed-point, PRICE_SCALE = 100'000'000
Quantity_t    = uint64_t
OrderId_t     = uint64_t    // Exchange-assigned order ID
ClOrdId_t     = uint64_t    // Client order ID
SymbolIndex_t = uint32_t    // Instrument identifier
TradeId_t     = uint64_t
SessionId_t   = uint16_t    // Client session identifier
MemberId_t    = uint16_t    // Clearing member identifier
Timestamp_ns  = uint64_t    // Nanosecond timestamp
```

**Price conversion:**
```cpp
Price_t px = toFixedPrice(150.25);  // → 15'025'000'000
double  d  = toDouble(px);          // → 150.25
```

### 6.3 Order & Trade Structs

```
 Order (88 bytes, packed)                Trade (72 bytes, packed)
 ═══════════════════════                 ═══════════════════════
 orderId      : uint64                   tradeId       : uint64
 clOrdId      : uint64                   symbolIdx     : uint32
 symbolIdx    : uint32                   price         : int64
 side         : uint8  (Buy=1,Sell=2)    quantity      : uint64
 ordType      : uint8  (Market=1,Lim=2)  buyOrderId    : uint64
 tif          : uint8  (Day/GTC/IOC/FOK) sellOrderId   : uint64
 price        : int64                    buyClOrdId    : uint64
 quantity     : uint64                   sellClOrdId   : uint64
 remainingQty : uint64                   matchTime     : uint64
 entryTime    : uint64                   buySessionId  : uint16
 sessionId    : uint16                   sellSessionId : uint16
 status       : uint8
```

---

## 7. Event System

Events flow between actors via `Event::Pipe`. Each event struct inherits `tredzone::Actor::Event`:

```
 ┌─────────────────────────────────────────────────────────────────────────────┐
 │                           EVENT CATALOG                                     │
 │                                                                             │
 │  NewOrderEvent         OEG → ME       New order submission                 │
 │  CancelOrderEvent      OEG → ME       Cancel resting order                │
 │  ModifyOrderEvent      OEG → ME       Modify price/qty (cancel-replace)   │
 │  ExecReportEvent       ME → OEG       Ack, fill, partial, reject          │
 │  TradeEvent            ME → MDG/CH    Trade execution                      │
 │  BookUpdateEvent       ME → MDG/AI    BBO + depth snapshot                │
 │  RecoveryFragmentEvent ME → Persist   Recovery persistence                │
 │                                                                             │
 │  Direction Key:                                                             │
 │  OEG = OEGActor       ME = MECoreActor     MDG = MDGActor                │
 │  CH = ClearingHouse    AI = AITraderActor                                  │
 └─────────────────────────────────────────────────────────────────────────────┘
```

### Event::Pipe Usage Pattern

```cpp
// In MECoreActor constructor:
oePipe_ = Event::Pipe(*this, oeGatewayId);   // pipe to OEG
mdPipe_ = Event::Pipe(*this, marketDataId);   // pipe to MDG

// Pushing an event (lock-free, zero-copy in same-core mode):
oePipe_.push<ExecReportEvent>(report, sessionId);
mdPipe_.push<TradeEvent>(trade);
```

---

## 8. Order Book & Matching

### Book (`src/common/Book.hpp`)

The `Book` class implements price-time priority matching:

```
 BIDS (sorted descending)              ASKS (sorted ascending)
 ════════════════════════              ════════════════════════

 Price    │ Orders (FIFO)              Price    │ Orders (FIFO)
 ─────────┼──────────────              ─────────┼──────────────
 155.00   │ [100] [50]                 157.00   │ [200]
 154.50   │ [200]                      158.00   │ [100] [150]
 154.00   │ [75] [100] [25]            159.00   │ [300]
 153.00   │ [500]                      160.00   │ [50]

 std::map<Price_t,                     std::map<Price_t,
   vector<Order>,                        vector<Order>,
   std::greater<>>                       std::less<>>
```

**Matching algorithm:**

```
 Incoming BUY @ 158.00, qty=250
 ──────────────────────────────

 Step 1: Match vs best ask 157.00 (qty=200)
         → Trade: 200 @ 157.00
         → Ask level 157.00 exhausted

 Step 2: Match vs next ask 158.00 (qty=100 + 150)
         → Trade: 50 @ 158.00 (from first order, partial)
         → Remaining incoming qty = 0 → DONE

 Step 3: Incoming fully filled
         → ExecReport: status=Filled, filledQty=250
```

**Order types:**
- `Limit`: rests on book if no match; price-time priority
- `Market`: sweeps all levels; never rests (IOC behavior)

**Time-in-force:**
- `Day`: rests until end of session
- `IOC`: fill what's available, cancel remainder
- `FOK`: fill all or reject entirely

### MECoreActor (`src/actors/MECoreActor.hpp`)

```
 NewOrderEvent ──► MECoreActor.onEvent()
                        │
                        ├── RecoveryProxy.cause(persistenceId, order, λ)
                        │     └── Fragment persisted to store
                        │
                        ├── book_.newOrder(order, onTrade, onExec)
                        │     ├── matchBuy() / matchSell()
                        │     │     └── for each fill: onTrade(trade)
                        │     └── onExec(report)
                        │
                        ├── oePipe_.push<ExecReportEvent>(report, session)
                        ├── mdPipe_.push<TradeEvent>(trade)         [per fill]
                        ├── mdPipe_.push<BookUpdateEvent>(snapshot)  [if book changed]
                        └── chPipe_.push<TradeEvent>(trade)         [per fill, if CH wired]
```

---

## 9. Recovery & IACA

### RecoveryProxy (`src/recovery/RecoveryProxy.hpp`)

```
 ┌──────────────────────────────────────────────────────────────────────┐
 │  RecoveryProxy (per MECoreActor)                                     │
 │                                                                      │
 │  cause(persistenceId, payload, callback)                             │
 │    → persist Fragment to FragmentStore                               │
 │    → callback returns nextCount (# children expected)               │
 │    → always executes (Master AND Mirror)                            │
 │                                                                      │
 │  effect(fn, args...)                                                 │
 │    → executes fn only on MASTER                                     │
 │    → side effects: send ExecReports, publish trades                 │
 │                                                                      │
 │  recoveryEffect(fn, args...)                                         │
 │    → executes fn only on MIRROR (during failover replay)            │
 │                                                                      │
 │  ┌────────────────┐            ┌────────────────┐                   │
 │  │  MASTER         │            │  MIRROR         │                   │
 │  │  cause()  ✓     │  ◄──────►  │  cause()  ✓     │                   │
 │  │  effect() ✓     │  Fragment   │  effect() ✗     │                   │
 │  │  recEff() ✗     │  Store      │  recEff() ✓     │                   │
 │  └────────────────┘  (shared)   └────────────────┘                   │
 └──────────────────────────────────────────────────────────────────────┘
```

### IACA Fragment Chains (`src/iaca/IacaAggregator.hpp`)

```
 Chain Completion Algorithm:
 ══════════════════════════

 Fragment 1 (ROOT: NEW_ORDER_BUY)       nextCount = 2
     ├── Fragment 2 (ACK_DATA)          nextCount = 0
     └── Fragment 3 (TRADE_DATA)        nextCount = 0

 Completion check:
   sum(nextCount) = 2 + 0 + 0 = 2
   total fragments = 3
   complete when: sum == total - 1     →  2 == 3 - 1  ✓

 On completion:
   → NewOrderHandler fires
   → Generates IA SBE message (future: IDS/PTB/SATURN)
```

---

## 10. Kafka Bus

### Overview

The Kafka Bus (`src/persistence/KafkaBus.hpp`) mirrors the Optiq KFK (Kafka Bus) that connects the Matching Engine to downstream consumers: MDG, PTB, Clearing, IDS, and SATURN.

When `EUNEX_USE_KAFKA` is enabled at compile time and `EUNEX_KAFKA_BROKERS` is set at runtime, the bus publishes events to Kafka topics in real time. When disabled, a no-op stub compiles in its place so the engine runs standalone.

### Topics

```
  Topic                        Content                  Key
  ─────────────────────────────────────────────────────────────
  eunex.orders                 Raw Order structs        symbolIdx
  eunex.trades                 Trade structs            symbolIdx
  eunex.market-data            BBO snapshots            symbolIdx
  eunex.recovery.fragments     Recovery fragments       originId:originKey
  eunex.control                Control messages         (reserved)
```

### Architecture

```
  MECoreActor (per symbol)
       │
       ├─ onEvent(NewOrderEvent)
       │     ├─ kafka_->publishOrder(order)         → eunex.orders
       │     ├─ onTrade callback:
       │     │     ├─ kafka_->publishTrade(trade)    → eunex.trades
       │     │     └─ mdPipe_ / chPipe_ (actors)
       │     └─ publishBookUpdate()
       │           └─ kafka_->publishMarketData(...) → eunex.market-data
       │
       └─ KafkaBus* kafka_  (nullptr when disabled)
```

### Compile-Time Toggle

```cmake
cmake .. -DEUNEX_USE_KAFKA=ON    # requires librdkafka-dev
cmake .. -DEUNEX_USE_KAFKA=OFF   # no-op stub (default)
```

### Runtime Configuration

Set `EUNEX_KAFKA_BROKERS` environment variable:

```bash
export EUNEX_KAFKA_BROKERS=kafka:9092
./eunex_me
```

The engine prints Kafka connection status at startup and publishes cumulative stats (orders, trades, market-data messages) every 10 rounds.

### Docker Deployment

`docker/docker-compose.yml` runs Kafka in KRaft mode (no ZooKeeper) using `apache/kafka:3.9.0`, creates all topics via `kafka-init`, then starts the engine with `EUNEX_KAFKA_BROKERS=kafka:9092`.

```bash
cd docker
docker compose up --build
```

---

## 11. FIX Protocol Gateway

### FIXAcceptorActor (`src/actors/FIXAcceptorActor.hpp`)

```
 ┌──────────────────────────────────────────────────────────────────────────┐
 │  FIXAcceptorActor (TCP :9001)                                            │
 │                                                                          │
 │  ┌─ Accept Thread ──────────────────────────────────────────────────┐    │
 │  │  while(running):                                                  │    │
 │  │    sock = accept(listenSock)                                      │    │
 │  │    create FIXSession { sock, sessionId, senderCompId }           │    │
 │  │    spawn client recv thread                                       │    │
 │  └───────────────────────────────────────────────────────────────────┘    │
 │                                                                          │
 │  ┌─ Client Recv Thread (per session) ───────────────────────────────┐    │
 │  │  while(loggedOn):                                                 │    │
 │  │    data = recv(sock)                                              │    │
 │  │    msgs = parseFIXMessages(data)                                  │    │
 │  │    for msg in msgs:                                               │    │
 │  │      switch(msg[35]):                                             │    │
 │  │        "A" → handleLogon()                                        │    │
 │  │        "D" → handleNewOrderSingle() → push NewOrderEvent → OEG  │    │
 │  │        "F" → handleCancelRequest()  → push CancelOrderEvent     │    │
 │  │        "G" → handleCancelReplace()  → push ModifyOrderEvent     │    │
 │  │        "5" → logout                                               │    │
 │  └───────────────────────────────────────────────────────────────────┘    │
 │                                                                          │
 │  ┌─ Actor Thread (onEvent) ─────────────────────────────────────────┐    │
 │  │  onEvent(ExecReportEvent):                                        │    │
 │  │    find session by sessionId                                      │    │
 │  │    build FIX ExecutionReport (35=8)                               │    │
 │  │    send() over TCP to client                                      │    │
 │  └───────────────────────────────────────────────────────────────────┘    │
 └──────────────────────────────────────────────────────────────────────────┘
```

**Supported FIX messages:**

| Tag 35 | Message                     | Direction |
|--------|-----------------------------|-----------|
| A      | Logon                       | Inbound   |
| 5      | Logout                      | Inbound   |
| 0      | Heartbeat                   | Inbound   |
| D      | NewOrderSingle              | Inbound   |
| F      | OrderCancelRequest          | Inbound   |
| G      | OrderCancelReplaceRequest   | Inbound   |
| 8      | ExecutionReport             | Outbound  |

**Symbol mapping:**

| Symbol String | SymbolIndex_t |
|---------------|---------------|
| AAPL          | 1             |
| MSFT          | 2             |
| GOOGL         | 3             |
| EURO50        | 4             |

---

## 12. Clearing House

### ClearingHouseActor (`src/actors/ClearingHouseActor.hpp`)

```
 ┌──────────────────────────────────────────────────────────────────────────┐
 │  ClearingHouseActor                                                      │
 │                                                                          │
 │  Session-to-Member Mapping:                                             │
 │  ┌──────────────────────────────────────────────────────────────────┐    │
 │  │  SessionId 100-109 ──► MemberId 1-10  (FIX gateway clients)    │    │
 │  │  SessionId 200-209 ──► MemberId 1-10  (AI traders)             │    │
 │  └──────────────────────────────────────────────────────────────────┘    │
 │                                                                          │
 │  On TradeEvent:                                                         │
 │  ┌──────────────────────────────────────────────────────────────────┐    │
 │  │  1. Resolve buySessionId → buyMember                             │    │
 │  │  2. Resolve sellSessionId → sellMember                           │    │
 │  │  3. Buy side:                                                    │    │
 │  │     capital -= price × quantity                                  │    │
 │  │     holdings[symbol].quantity += quantity                        │    │
 │  │     holdings[symbol].avgCost updated (weighted average)         │    │
 │  │  4. Sell side:                                                   │    │
 │  │     capital += price × quantity                                  │    │
 │  │     holdings[symbol].quantity -= quantity                        │    │
 │  │  5. Both sides: tradeCount++                                    │    │
 │  └──────────────────────────────────────────────────────────────────┘    │
 │                                                                          │
 │  Members (10):                                                          │
 │  ┌────────┬──────────┬─────────────────┬────────┐                       │
 │  │ Name   │ Capital  │ Holdings        │ Trades │                       │
 │  ├────────┼──────────┼─────────────────┼────────┤                       │
 │  │ MBR01  │ 100000.0 │ {sym→qty,cost}  │ 0      │                       │
 │  │ MBR02  │ 100000.0 │ {sym→qty,cost}  │ 0      │                       │
 │  │ ...    │ ...      │ ...             │ ...    │                       │
 │  │ MBR10  │ 100000.0 │ {sym→qty,cost}  │ 0      │                       │
 │  └────────┴──────────┴─────────────────┴────────┘                       │
 │                                                                          │
 │  getLeaderboard():                                                      │
 │    sorted by capital descending                                         │
 │    returns: memberId, name, capital, pnl, tradeCount, holdingCount     │
 │    thread-safe (mutex) for Python bridge reads                          │
 └──────────────────────────────────────────────────────────────────────────┘
```

---

## 13. AI Trading Members

### AITraderActor (`src/actors/AITraderActor.hpp`)

```
 ┌──────────────────────────────────────────────────────────────────────────┐
 │  AITraderActor — 10 automated trading members                            │
 │                                                                          │
 │  Members:                                                               │
 │  ┌────────┬───────────┬────────────────┬──────────────┐                  │
 │  │ Name   │ SessionId │ Strategy       │ Description  │                  │
 │  ├────────┼───────────┼────────────────┼──────────────┤                  │
 │  │ MBR01  │ 200       │ Momentum       │ Follow trend │                  │
 │  │ MBR02  │ 201       │ MeanReversion  │ Fade moves   │                  │
 │  │ MBR03  │ 202       │ Random         │ Noise trader │                  │
 │  │ MBR04  │ 203       │ Momentum       │              │                  │
 │  │ ...    │ ...       │ (rotates)      │              │                  │
 │  │ MBR10  │ 209       │ MeanReversion  │              │                  │
 │  └────────┴───────────┴────────────────┴──────────────┘                  │
 │                                                                          │
 │  Strategy Details:                                                      │
 │  ┌──────────────────────────────────────────────────────────────────┐    │
 │  │                                                                  │    │
 │  │  MOMENTUM:                                                       │    │
 │  │    if last N prices trending up → BUY (follow the trend)        │    │
 │  │    if last N prices trending down → SELL                        │    │
 │  │    price = BBO ± small offset                                   │    │
 │  │                                                                  │    │
 │  │  MEAN REVERSION:                                                 │    │
 │  │    if price > moving average → SELL (expect revert)             │    │
 │  │    if price < moving average → BUY                              │    │
 │  │    price = BBO ± small offset                                   │    │
 │  │                                                                  │    │
 │  │  RANDOM:                                                         │    │
 │  │    random side (50/50), random qty (10-100)                     │    │
 │  │    price around midpoint with random spread                     │    │
 │  │                                                                  │    │
 │  └──────────────────────────────────────────────────────────────────┘    │
 │                                                                          │
 │  Data Sources:                                                          │
 │    • BookUpdateEvent → BBO (bestBid, bestAsk) per symbol               │
 │    • TradeEvent → price history (last 50 prices) per symbol            │
 │    • ExecReportEvent → order fill confirmations                        │
 │                                                                          │
 │  Output:                                                                │
 │    • NewOrderEvent → OEGActor (via Event::Pipe)                        │
 │    • ~30s intervals via Actor::Callback                                │
 └──────────────────────────────────────────────────────────────────────────┘
```

---

## 14. Project Structure

```
 EuNEx/
 ├── CMakeLists.txt                    Build configuration
 ├── README.md                         Project overview
 │
 ├── src/
 │   ├── main.cpp                      Engine entry point, actor wiring
 │   │
 │   ├── common/
 │   │   ├── Types.hpp                 Core types (Price_t, Order, Trade, etc.)
 │   │   ├── Book.hpp                  Order book interface
 │   │   └── Book.cpp                  Price-time priority matching engine
 │   │
 │   ├── engine/
 │   │   └── SimplxShim.hpp            Actor framework (Simplx API compatible)
 │   │
 │   ├── actors/
 │   │   ├── Events.hpp                All inter-actor event definitions
 │   │   ├── OEGActor.hpp/cpp          Order Entry Gateway
 │   │   ├── MECoreActor.hpp/cpp       Matching Engine core (per symbol)
 │   │   ├── MDGActor.hpp/cpp          Market Data Gateway
 │   │   ├── FIXAcceptorActor.hpp/cpp  FIX 4.4 TCP protocol gateway
 │   │   ├── ClearingHouseActor.hpp/cpp Trade clearing & member positions
 │   │   └── AITraderActor.hpp/cpp     Automated trading members
 │   │
 │   ├── net/
 │   │   └── SocketCompat.hpp          Cross-platform socket abstraction
 │   │
 │   ├── recovery/
 │   │   ├── RecoveryProxy.hpp/cpp     Master/Mirror recovery gating
 │   │   └── (FragmentStore)           In-memory persistence (→ Kafka future)
 │   │
 │   ├── iaca/
 │   │   ├── Fragment.hpp              IACA fragment definitions
 │   │   ├── IacaAggregator.hpp/cpp    Fragment chain assembly & completion
 │   │   └── (handlers)                IA message generators
 │   │
 │   └── persistence/
 │       ├── PersistenceStore.hpp       Store interface
 │       └── KafkaStore.hpp             Kafka adapter (optional, librdkafka)
 │
 ├── tests/
 │   ├── test_orderbook.cpp            Book unit tests (26 cases)
 │   ├── test_matching_engine.cpp      ME integration tests
 │   ├── test_threaded_engine.cpp      Multi-threaded actor tests
 │   ├── test_clearing_house.cpp       Clearing house tests (7 cases)
 │   ├── test_fix_gateway.cpp          FIX gateway tests (5 cases)
 │   └── test_ai_trader.cpp            AI trader tests (6 cases)
 │
 ├── examples/
 │   ├── ping_pong.cpp                 Basic actor communication demo
 │   └── simple_match.cpp              Simple order matching demo
 │
 └── docs/
     ├── developers-guide.md           This document
     └── process-diagram.md            Detailed Optiq architecture diagrams
```

---

## 15. Build & Test

### Prerequisites

- **C++20 compiler**: MSVC 19.30+, GCC 12+, Clang 15+
- **CMake**: 3.16+
- **Optional**: librdkafka (for Kafka persistence)

### Build Commands

```bash
# Configure (default: tests ON, Kafka OFF)
cmake -B build -DEUNEX_BUILD_TESTS=ON

# Build
cmake --build build --config Release

# Run tests
cd build && ctest -C Release --output-on-failure

# Run engine
./build/Release/eunex_me
```

### CMake Options

| Option               | Default | Description                        |
|---------------------|---------|------------------------------------|
| `EUNEX_USE_SIMPLX`  | OFF     | Use real Simplx framework          |
| `EUNEX_USE_KAFKA`   | OFF     | Enable Kafka persistence           |
| `EUNEX_BUILD_TESTS` | ON      | Build test binaries                |
| `EUNEX_BUILD_EXAMPLES` | ON   | Build example binaries             |

### Test Suites

```
 ┌───────────────────────────────────────────────────────────────┐
 │  Test Suite              │ Cases │ What it verifies            │
 ├──────────────────────────┼───────┼─────────────────────────────┤
 │  OrderBookTest           │  26   │ Book matching, all TIFs,    │
 │                          │       │ multi-level sweeps, cancels │
 ├──────────────────────────┼───────┼─────────────────────────────┤
 │  MatchingEngineTest      │   -   │ MECoreActor event handling  │
 ├──────────────────────────┼───────┼─────────────────────────────┤
 │  ThreadedEngineTest      │   -   │ Multi-core Engine, mailbox  │
 ├──────────────────────────┼───────┼─────────────────────────────┤
 │  ClearingHouseTest       │   7   │ Capital, holdings, P&L,     │
 │                          │       │ session mapping, leaderboard│
 ├──────────────────────────┼───────┼─────────────────────────────┤
 │  FIXGatewayTest          │   5   │ Symbol mapping, actor       │
 │                          │       │ lifecycle, OEG routing      │
 ├──────────────────────────┼───────┼─────────────────────────────┤
 │  AITraderTest            │   6   │ Strategy execution, multi-  │
 │                          │       │ symbol, clearing integration│
 └───────────────────────────────────────────────────────────────┘
```

---

## 16. Configuration

### Runtime Configuration (main.cpp)

| Parameter            | Value     | Description                          |
|---------------------|-----------|--------------------------------------|
| FIX port            | 9001      | FIXAcceptorActor TCP listen port     |
| Symbols             | 4         | AAPL(1), MSFT(2), GOOGL(3), EURO50(4) |
| AI members          | 10        | MBR01-MBR10                          |
| Initial capital     | 100,000.0 | Per clearing member                  |
| AI trade interval   | ~3s       | Per round in main loop               |
| Price scale         | 10^8      | Fixed-point decimal places           |

### Seed Orders

The engine pre-populates order books with spread-defining orders:

```
 AAPL:   Sell 155.00/100, 154.00/200  |  Buy 153.00/150, 152.00/100
 MSFT:   Sell 325.00/100, 324.00/150  |  Buy 323.00/200, 322.00/100
 GOOGL:  Sell 142.00/100, 141.00/200  |  Buy 140.00/150, 139.00/100
 EURO50: Sell 5050.00/50, 5040.00/80  |  Buy 5030.00/60, 5020.00/40
```

---

## 17. Extending EuNEx

### Adding a New Symbol

1. Define a new `SymbolIndex_t` constant in `main.cpp`
2. Create a `MECoreActor` for it, passing OEG, MDG, and CH actor IDs
3. Register it with `oeGateway->mapSymbol(newSym, bookActor->getActorId())`
4. Add it to `AITraderActor`'s symbol list
5. Add seed orders if desired

### Adding a New Actor

1. Create `src/actors/MyActor.hpp` inheriting `tredzone::Actor`
2. Register event handlers in constructor: `registerEventHandler<EventT>(*this)`
3. Create `Event::Pipe` members for each destination actor
4. Add `.cpp` to `EUNEX_CORE_SOURCES` in `CMakeLists.txt`
5. Wire into topology in `main.cpp`

### Adding a New Event Type

1. Define the struct in `src/actors/Events.hpp` inheriting `tredzone::Actor::Event`
2. Add `onEvent(const MyEvent&)` to receiving actors
3. Register handler: `registerEventHandler<MyEvent>(*this)`
4. Push via pipe: `pipe.push<MyEvent>(args...)`

### Adding a New Trading Strategy

1. Add enum value to `Strategy` in `AITraderActor.hpp`
2. Implement `strategyMyStrategy()` method
3. Add case to `submitOrder()` switch
4. Assign to desired members in `initMembers()`

### Future Roadmap

```
 Current (v0.4)                    Planned
 ══════════════                    ═══════════════════════════════════

 ✓ Limit + Market orders           □ Stop, Pegged, Mid-Point, Iceberg
 ✓ IOC, FOK, Day                   □ GTD, GTC, VFU, VFCU
 ✓ Continuous matching              □ Trading phases (pre-open, close)
 ✓ FIX 4.4 gateway                 □ FIX 5.0 SP2 + SBE binary
 ✓ In-memory persistence           □ Kafka persistence
 ✓ SimplxShim (emulation)          □ Real Simplx multi-core
 ✓ 4 symbols                       □ Multi-segment, partitions
 ✓ Single partition                □ Cross-partition routing
 ✓ Basic clearing                  □ EuroCCP/LCH integration
 ✓ IACA fragments                  □ IACA FINISH + COPY + IDS
 ✓ Python bridge (JSON)            □ SBE multicast MDG
                                    □ SATURN ARM (MiFID II RTS 22)
                                    □ PTB (Post-Trade Box)
                                    □ Uncrossing algorithm
                                    □ Price collars & reservations
```

---

*Generated for EuNEx v0.4.0 — Euronext Optiq Architecture Simulator*
