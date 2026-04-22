# EuNEx Process Diagram

## End-to-End Order Flow (Optiq-style)

```
 ┌─────────────────────────────────────────────────────────────────────────────────────┐
 │                              EXTERNAL CLIENTS                                      │
 │                     FIX 5.0 SP2 / SBE Binary Protocol                              │
 └────────────────────────────────┬────────────────────────────────────────────────────┘
                                  │
                                  ▼
 ┌────────────────────────────────────────────────────────────────────────────────────┐
 │  OEGActor  (Core 0)                                    ← Optiq: OEG/OEActor│
 │ ┌──────────────────────────────────────────────────────────────────────────────┐   │
 │ │  • Accepts NewOrder / Cancel / Modify requests                              │   │
 │ │  • Validates session, routes by SymbolIndex → correct MECoreActor        │   │
 │ │  • Sends ExecutionReports back to client (ack, fill, reject)                │   │
 │ └──────────────────────────────────────────────────────────────────────────────┘   │
 └────────────────────────────────┬────────────────────────────────────────────────────┘
                                  │ NewOrderEvent / CancelOrderEvent / ModifyOrderEvent
                                  │ (Simplx Event::Pipe — lock-free cross-core delivery)
                                  ▼
 ┌────────────────────────────────────────────────────────────────────────────────────┐
 │  MECoreActor  (Core 1)  — one per symbol      ← Optiq: LogicalCoreActor + Book│
 │ ┌──────────────────────────────────────────────────────────────────────────────┐   │
 │ │                        RECOVERY CAUSE                                       │   │
 │ │  RecoveryProxy.cause(persistenceId, order, callback)                        │   │
 │ │  • Persists incoming event to FragmentStore (→ Kafka in production)          │   │
 │ │  • Assigns chainId + sequence for IACA tracking                             │   │
 │ └──────────────────────────────────┬───────────────────────────────────────────┘   │
 │                                    ▼                                               │
 │ ┌──────────────────────────────────────────────────────────────────────────────┐   │
 │ │                        MATCHING ENGINE                                      │   │
 │ │  Book.newOrder() / cancelOrder() / modifyOrder()                       │   │
 │ │  • Price-time priority matching (std::map<Price, std::deque<Order>>)        │   │
 │ │  • FOK: reject if insufficient liquidity                                    │   │
 │ │  • IOC: fill what's available, cancel remainder                             │   │
 │ │  • Market: match at any price, never rests on book                          │   │
 │ │  • Multi-level sweep across price levels                                    │   │
 │ └──────┬─────────────────────────────────────────────────┬─────────────────────┘   │
 │        │                                                 │                         │
 │        ▼                                                 ▼                         │
 │ ┌────────────────────┐                    ┌────────────────────────────────────┐   │
 │ │  IACA CAUSE        │                    │  RECOVERY EFFECT (Master only)    │   │
 │ │  IacaFragment:     │                    │  RecoveryProxy.effect(callback)   │   │
 │ │  • BOOK root frag  │                    │  • Sends ExecReport → OEG  │   │
 │ │  • ACK child frag  │                    │  • Sends TradeEvent → MarketData │   │
 │ │  • TRADE child frag│                    │  • Sends BookUpdate → MarketData │   │
 │ │  (nextCount check) │                    │  • Mirror skips all effects      │   │
 │ └────────┬───────────┘                    └─────────┬──────────┬─────────────┘   │
 │          │                                          │          │                   │
 └──────────┼──────────────────────────────────────────┼──────────┼───────────────────┘
            │                                          │          │
            ▼                                          │          │
 ┌──────────────────────────┐                          │          │
 │  IacaAggregator          │                          │          │
 │  ← Optiq: IacaActor      │                          │          │
 │ ┌──────────────────────┐ │                          │          │
 │ │ Collects fragments   │ │                          │          │
 │ │ per chainId          │ │                          │          │
 │ │ Detects completion   │ │                          │          │
 │ │ (sum nextCount ==    │ │                          │          │
 │ │  total fragments -1) │ │                          │          │
 │ │ Fires handler →      │ │                          │          │
 │ │ IA SBE message       │ │                          │          │
 │ └──────────────────────┘ │                          │          │
 └──────────┬───────────────┘                          │          │
            │ IA message                               │          │
            ▼                                          │          │
 ┌──────────────────────────┐              ┌───────────▼──────────▼──────────────────┐
 │  Future: IDS / PTB /     │              │  MDGActor  (Core 2)              │
 │  SATURN / Clearing       │              │  ← Optiq: MDLimitLogicalCoreHandler     │
 │ ┌──────────────────────┐ │              │ ┌────────────────────────────────────┐   │
 │ │ IDS: Intraday Data   │ │              │ │ • Maintains BBO snapshot per sym  │   │
 │ │ PTB: Post-Trade Bus  │ │              │ │ • Stores recent trades            │   │
 │ │ SATURN: ARM reporting│ │              │ │ • Publishes BookUpdateEvent       │   │
 │ │ Clearing: LCH/CC&G   │ │              │ │   (bestBid, bestAsk, depths)      │   │
 │ └──────────────────────┘ │              │ │ → Future: multicast SBE feed      │   │
 │  (not yet implemented)   │              │ └────────────────────────────────────┘   │
 └──────────────────────────┘              └─────────────────────────────────────────┘
```

## Master / Mirror High Availability

```
 ┌─────────────────────────────────────────┐     ┌─────────────────────────────────┐
 │           MASTER NODE                   │     │         MIRROR NODE             │
 │                                         │     │                                 │
 │  RecoveryProxy(isMaster=true)           │     │  RecoveryProxy(isMaster=false)  │
 │                                         │     │                                 │
 │  cause() → persist + run callback  ✓    │     │  cause() → persist + callback ✓│
 │  effect() → execute side-effects   ✓    │────▶│  effect() → SKIP (no-op)   ✗   │
 │  recoveryEffect() → SKIP          ✗    │     │  recoveryEffect() → execute ✓  │
 │                                         │     │                                 │
 │  ┌─────────────────────────────────┐    │     │  ┌───────────────────────────┐  │
 │  │ FragmentStore (shared memory)   │────┼─────┼─▶│ Same FragmentStore        │  │
 │  │ → Kafka topic in production     │    │     │  │ (Kafka consumer replay)   │  │
 │  └─────────────────────────────────┘    │     │  └───────────────────────────┘  │
 └─────────────────────────────────────────┘     └─────────────────────────────────┘
                                                  On failover: Mirror becomes Master,
                                                  replays FragmentStore, resumes effects
```

## IACA Fragment Chain Example (Buy order that matches)

```
 Chain #42: BUY 60 @ 50.00 (matches resting SELL 100 @ 50.00)

 ┌─────────────────────────────┐
 │  Fragment 1 (ROOT)          │
 │  origin: BOOK:1:seq         │
 │  previousOrigin: null       │
 │  causeId: NEW_ORDER_BUY     │
 │  nextCount: 2               │──────┐
 └─────────────────────────────┘      │
          │                            │
          ├───────────────┐            │  2 children expected
          ▼               ▼            │  2 children found → COMPLETE
 ┌─────────────┐  ┌─────────────┐     │
 │ Fragment 2  │  │ Fragment 3  │     │
 │ ACK_DATA    │  │ TRADE_DATA  │     │
 │ prev: ROOT  │  │ prev: ROOT  │     │
 │ nextCount: 0│  │ nextCount: 0│     │
 └─────────────┘  └─────────────┘     │
                                       │
 sum(nextCount) = 2 + 0 + 0 = 2  ─────┘
 total fragments = 3
 complete when: sum == total - 1  ✓ (2 == 3-1)
           ▼
 ┌─────────────────────────┐
 │ Handler fires:          │
 │ NewOrderHandler →       │
 │ Generate IA SBE message │
 └─────────────────────────┘
```

## Component Mapping: StockEx → EuNEx → Optiq Production

```
 StockEx (Python/Kafka)          EuNEx (C++/Simplx shim)         Optiq (Production)
 ═══════════════════════         ═══════════════════════          ══════════════════

 fix_oeg_server.py         ───▶  OEGActor                  ───▶  OEActor (FIX/SBE)
   Kafka 'orders' topic    ───▶    Event::Pipe              ───▶    Simplx cross-core
   session mgmt            ───▶    submitNewOrder()          ───▶    FIX 5.0 SP2 acceptor

 matcher.py                ───▶  MECoreActor                ───▶  LogicalCoreActor
   match_order()           ───▶    Book::newOrder()          ───▶    RecoveryCause→IACA→Book
   handle_cancel()         ───▶    Book::cancelOrder()       ───▶    CancelOrderData
   handle_amend()          ───▶    Book::modifyOrder()       ───▶    ModifyOrderData
   Kafka 'trades' topic    ───▶    TradeEvent via Pipe       ───▶    IACA fragment chain

 dashboard.py (SSE)        ───▶  MDGActor                   ───▶  MDLimitLogicalCoreHandler
   /orderbook/<sym>        ───▶    BookUpdateEvent           ───▶    PublishLimitUpdateRequest
   /trades                 ───▶    TradeEvent list           ───▶    IA→SBE multicast

 database.py (SQLite)      ───▶  RecoveryProxy (memory)     ───▶  RecoveryProxy → Kafka
   save_trade()            ───▶    FragmentStore::append()   ───▶    PersistenceAgent produce

 (none)                    ───▶  IacaAggregator             ───▶  IacaAggregatorActor
                           ───▶    FragmentChain             ───▶    CoherentFragmentChain
                           ───▶    FragmentHandler           ───▶    FastNewOrderHandler

 (none)                    ───▶  (future)                   ───▶  SATURN/ARM
 (none)                    ───▶  (future)                   ───▶  IDS (Intraday Data)
 (none)                    ───▶  (future)                   ───▶  PTB (Post-Trade Bus)
 (none)                    ───▶  (future)                   ───▶  Clearing (LCH/CC&G)
```

## Data Flow Summary

```
 1. Client ──FIX/SBE──▶ OEGActor ──Event::Pipe──▶ MECoreActor
                              ▲                              │
                              │                              ├─ RecoveryProxy.cause() → FragmentStore
                              │                              ├─ Book.match()
                              │                              ├─ IACA fragments → IacaAggregator
                              │                              │
                 ExecReport ◀─┘                              ├─ effect() [Master only]:
                                                             │    ├─ ExecReport → OEG → Client
                                                             │    ├─ TradeEvent → MDGActor
                                                             │    └─ BookUpdate → MDGActor
                                                             │
                                                             └─ IacaAggregator
                                                                  └─ complete chain → IA message
                                                                       └─ (future) IDS/PTB/SATURN
```

---

## Future Expansion: Full Optiq Production Topology

The diagram below shows the complete Optiq production trade path (from `Trades paths.drawio`
and the Optiq Overview/SATURN presentations) with EuNEx components mapped where they exist
and `[FUTURE]` markers where implementation is needed.

```
 ┌──────────────────────────────────────────────────────────────────────────────────────────┐
 │                             TRADING MEMBERS (Clients)                                   │
 │               FIX 5.0 SP2 / SBE / TCS OTC / SATURN Web / EMS / DAS                     │
 └───────────┬──────────────┬──────────────────────┬───────────────────┬────────────────────┘
             │              │                      │                   │
             ▼              ▼                      ▼                   ▼
 ┌───────────────┐  ┌──────────────┐    ┌──────────────────┐  ┌────────────────┐
 │  OEG.n.BOTH   │  │  TCS.IN/OUT  │    │  SATURN Web/API  │  │  EMS / DAS     │
 │  (OEG   │  │  [FUTURE]    │    │  [FUTURE]        │  │  [FUTURE]      │
 │   Actor) ✓    │  │  Trade       │    │  External trade  │  │  Execution     │
 │               │  │  Capture     │    │  declarations    │  │  Mgmt System   │
 └───────┬───────┘  │  Service     │    └────────┬─────────┘  └────────────────┘
         │          └──────┬───────┘             │
         │ ord             │ Trade Decl          │
         ▼                 ▼                     │
 ┌────────────────────────────────────────────────────────────────────────────────────────┐
 │  ME / Trading Chain                                                                    │
 │ ┌────────────────────────────────────────────────────────────────────────────────────┐  │
 │ │  Book / LogCore#n  (MECoreActor) ✓                                              │  │
 │ │                                                                                    │  │
 │ │  ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐                    │  │
 │ │  │ Recovery Cause  │  │ Matching Engine │  │ IACA Inside     │                    │  │
 │ │  │ (RecoveryProxy  │  │ (Book) ✓   │  │ (IacaAggregator │                    │  │
 │ │  │  → Kafka) ✓     │  │                 │  │  partial) ✓     │                    │  │
 │ │  └────────┬────────┘  └────────┬────────┘  └────────┬────────┘                    │  │
 │ │           │                    │                     │                              │  │
 │ │           ▼                    ▼                     ▼                              │  │
 │ │  ┌──────────────────────────────────────────────────────────┐                      │  │
 │ │  │         Kafka Bus (KFK)  [FUTURE — currently in-memory]  │                      │  │
 │ │  └──┬──────┬──────┬──────┬──────┬──────┬──────┬──────┬──────┘                      │  │
 │ └─────┼──────┼──────┼──────┼──────┼──────┼──────┼──────┼────────────────────────────┘  │
 └───────┼──────┼──────┼──────┼──────┼──────┼──────┼──────┼──────────────────────────────┘
          │      │      │      │      │      │      │      │
          ▼      ▼      ▼      ▼      ▼      ▼      ▼      ▼
```

### Downstream Consumers from Kafka Bus

```
 ┌─────────────── Kafka Topics ──────────────────────────────────────────────────────┐
 │  recovery+audit trail │ MktUpdt,FullTradeInfo │ Trade Fill │ IACA fragments       │
 └───────┬───────────────┼──────────┬────────────┼─────┬─────┼──────────┬────────────┘
         │               │          │            │     │     │          │
         ▼               ▼          │            │     │     │          ▼
 ┌──────────────┐ ┌─────────────┐   │            │     │     │  ┌──────────────────┐
 │  PE DB       │ │ MDG         │   │            │     │     │  │ IACA FINISH      │
 │ [FUTURE]     │ │(MarketData  │   │            │     │     │  │ [FUTURE]         │
 │ Persistence  │ │ Actor) ✓    │   │            │     │     │  │ Completes chains │
 │ Engine DB    │ │             │   │            │     │     │  │ → IA SBE messages│
 │ (order state │ │ Multicast   │   │            │     │     │  └────────┬─────────┘
 │  snapshots)  │ │ SBE feed    │   │            │     │     │           │
 └──────────────┘ │ [FUTURE]    │   │            │     │     │           ▼
                  └─────────────┘   │            │     │     │  ┌──────────────────┐
                                    ▼            │     │     │  │ IACA COPY        │
                          ┌──────────────────┐   │     │     │  │ [FUTURE]         │
                          │ IDS              │   │     │     │  │ Copy to members  │
                          │ [FUTURE]         │   │     │     │  │ (DropCopy/DCG)   │
                          │ Intraday Data    │   │     │     │  └──────────────────┘
                          │ Service          │   │     │     │
                          │ (files to        │   │     │     │
                          │  regulators)     │   │     │     │
                          └──────────────────┘   │     │     │
                                                 ▼     │     │
                                    ┌────────────────┐ │     │
                                    │ PTB            │ │     │
                                    │ [FUTURE]       │ │     │
                                    │ Post-Trade Box │ │     │
                                    │ (admin,        │ │     │
                                    │  resilience)   │ │     │
                                    └───────┬────────┘ │     │
                                            │          │     │
                                            ▼          ▼     ▼
                              ┌────────────────────────────────────────────┐
                              │         CLEARING  [FUTURE]                 │
                              │ ┌──────────┐ ┌──────────┐ ┌────────────┐  │
                              │ │ EuroCCP  │ │ LCH Ltd  │ │ SIX Xclear │  │
                              │ └──────────┘ └──────────┘ └────────────┘  │
                              │     Enxt Clearing ← PTB2EC               │
                              └────────────────────────────────────────────┘
```

---

## Future Expansion: Trading Phases & Order Types

Based on the Euronext Trading Manual (Notice 4-01, Dec 2025).

### Trading Day Phases (not yet implemented in EuNEx)

```
 ┌─────────────────────────────────────────────────────────────────────────────────┐
 │                    CONTINUOUS TRADING DAY LIFECYCLE                              │
 │                                                                                 │
 │  ┌─────────────┐  ┌──────────┐  ┌────────────────┐  ┌──────────┐  ┌────────┐  │
 │  │ Pre-opening │  │ Opening  │  │     Main       │  │Pre-close │  │Closing │  │
 │  │ Call Phase  │──▶│Uncrossing│──▶│   Trading     │──▶│Call Phase│──▶│Uncross │  │
 │  │             │  │          │  │   Session      │  │          │  │        │  │
 │  │ Orders      │  │ Book     │  │ Continuous     │  │ Orders   │  │ Max    │  │
 │  │ accumulate, │  │ frozen,  │  │ matching,      │  │ accum.,  │  │ exec.  │  │
 │  │ IOP calc'd  │  │ IOP →    │  │ price-time     │  │ no       │  │ price  │  │
 │  │ continuously│  │ opening  │  │ priority       │  │ trades   │  │ algo   │  │
 │  │             │  │ price    │  │                │  │          │  │        │  │
 │  └─────────────┘  └──────────┘  └────────────────┘  └──────────┘  └───┬────┘  │
 │                                                                        │       │
 │                   ┌────────────────┐    ┌──────────────────────┐        │       │
 │                   │ AVD Orders     │    │ Trading-at-Last     │◀───────┘       │
 │                   │ [FUTURE]       │    │ [FUTURE]            │                │
 │                   │ Auction Volume │    │ Trade only at TAL   │                │
 │                   │ Discovery —    │    │ price (= last trade │                │
 │                   │ after each     │    │ or closing price)   │                │
 │                   │ uncrossing     │    │                     │                │
 │                   └────────────────┘    └──────────────────────┘                │
 └─────────────────────────────────────────────────────────────────────────────────┘
```

### Order Types Roadmap

```
 Currently Implemented (EuNEx)          To Be Added
 ═══════════════════════════            ═════════════════════════════════════════

 ✓ Limit orders                        □ Market-to-Limit orders
 ✓ Market orders (IOC)                    (converted to limit at best opposite)
 ✓ IOC (Immediate or Cancel)           □ Stop-Market orders
 ✓ FOK (Fill or Kill)                     (triggered when price crosses stop)
 ✓ Day validity                        □ Stop-Limit orders
 ✓ Cancel order                           (stop trigger + limit price)
 ✓ Modify order                        □ Primary Pegged orders
                                           (pegged to BBO, auto-reprice)
                                        □ Mid-Point orders
                                           (pegged to mid of BBO spread)
                                        □ Iceberg orders
                                           (displayed qty + hidden reserve)
                                        □ AVD orders (Auction Volume Discovery)
                                           (fill imbalance after uncrossing)
                                        □ GTD / GTC validity
                                           (Good Till Date / Good Till Cancel)
                                        □ VFU / VFCU validity
                                           (Valid For Uncrossing / Closing Uncr.)
```

### Uncrossing Algorithm (Opening / Closing)

```
 [FUTURE] Uncrossing price determination algorithm:

 Step 1: Maximum Execution Principle
         Find price P that maximizes executable volume

 Step 2: Minimum Surplus
         If tie → pick P with smallest order imbalance

 Step 3: Reference Price
         If still tie → pick P closest to previous reference price

 Step 4: Market Pressure
         If still tie → pick higher P if buy surplus, lower if sell surplus

 Currently EuNEx uses continuous matching only.
 Uncrossing requires:
   • TradingPhaseManager actor
   • Theoretical Opening Price (IOP) calculation
   • Book freeze during uncrossing
   • AVD order processing after uncrossing
```

### Price Collars & Reservations

```
 [FUTURE] Price protection mechanisms:

 ┌────────────────────────────────────────────────────────────┐
 │  DYNAMIC COLLARS                                          │
 │  Based on last traded price ± threshold %                 │
 │  Breach → order rejected or book reserved                 │
 │                                                           │
 │  STATIC COLLARS                                           │
 │  Based on reference price ± wider threshold %             │
 │  Breach during continuous → reservation + uncrossing      │
 │                                                           │
 │  RESERVATION                                              │
 │  Book frozen, enters call phase                           │
 │  Duration: configurable per trading group                 │
 │  After reservation → uncrossing to resume trading         │
 └────────────────────────────────────────────────────────────┘
```

---

## Future Expansion: Segments & Partitions

From the Optiq Architecture presentation.

```
 ┌─────────────────────────────────────────────────────────────────────┐
 │                    OPTIQ SEGMENTS                                   │
 │                                                                     │
 │  Cash                              Derivatives                      │
 │  ┌─────┐ ┌─────┐ ┌─────┐ ┌─────┐  ┌─────┐ ┌─────┐ ┌─────┐       │
 │  │ EQU │ │ ETF │ │ FXI │ │ WAR │  │ EQD │ │ IDD │ │ CMO │       │
 │  │Equit│ │Track│ │Bonds│ │Warr │  │Eq.  │ │Index│ │Commo│       │
 │  │ies  │ │ers  │ │Debt │ │ants │  │Deriv│ │Deriv│ │dity │       │
 │  └─────┘ └─────┘ └─────┘ └─────┘  └─────┘ └─────┘ └─────┘       │
 │                                                                     │
 │  ┌─────┐                                                           │
 │  │ BLK │  Block — large trades off main book                       │
 │  └─────┘                                                           │
 │                                                                     │
 │  Each segment has:                                                  │
 │    • Own OEG instances (OEG.n.BOTH)                                │
 │    • Own ME partitions                                              │
 │    • Own MDG multicast channels                                     │
 │                                                                     │
 │  EuNEx currently: single segment, single partition                  │
 │  [FUTURE] Multi-segment support via SegmentManager actor            │
 └─────────────────────────────────────────────────────────────────────┘

 ┌─────────────────────────────────────────────────────────────────────┐
 │                    PARTITIONS (Horizontal Scalability)               │
 │                                                                     │
 │  1 Partition = 1 Machine = 1 TRNODE                                │
 │                                                                     │
 │  ┌────────────┐  ┌────────────┐  ┌────────────┐  ┌────────────┐   │
 │  │ Partition 1│  │ Partition 2│  │ Partition 3│  │ Partition 4│   │
 │  │ Group 11   │  │ Group 16   │  │ Group 35   │  │ Group 37   │   │
 │  │ Group 12   │  │ Group 17   │  │ Group 44   │  │ ...        │   │
 │  │ Group 13   │  │ ...        │  │ ...        │  │            │   │
 │  └────────────┘  └────────────┘  └────────────┘  └────────────┘   │
 │                                                                     │
 │  Cross-partition orders → routed via OEG to correct partition      │
 │                                                                     │
 │  Trading Group = set of instruments with shared:                    │
 │    • Trading schedule (phases, times)                               │
 │    • Matching rules                                                 │
 │    • Collar parameters                                              │
 │    • Market segment or liquidity level                              │
 │                                                                     │
 │  Each Instrument:                                                   │
 │    • Identified by SymbolIndex                                      │
 │    • May have multiple order books                                  │
 │    • Inherits trading group attributes                              │
 │    • Can override: APF (Authorized Price Fluctuation)               │
 │                                                                     │
 │  EuNEx currently: MECoreActor per symbol, single "partition"    │
 │  [FUTURE] PartitionManager distributes symbols across nodes         │
 └─────────────────────────────────────────────────────────────────────┘
```

---

## Future Expansion: SATURN (Regulatory Reporting)

From the SATURN Overview v1.0.4 presentation.

```
 ┌────────────────────────────────────────────────────────────────────────────────────┐
 │                          SATURN SYSTEM  [FUTURE]                                   │
 │                                                                                    │
 │  ┌───────────────────────────────────────────────────────────────────────────────┐  │
 │  │  ARM — Approved Reporting Mechanism (MiFID II RTS 22)                        │  │
 │  │                                                                               │  │
 │  │  Trading Engine ──order msg──▶ Check Library ──▶ Reporting Tool ──▶ NCAs     │  │
 │  │       (ME)        (incl.        (MiFID II        (RTS 22           (AMF,     │  │
 │  │                    MiFID II      controls,        formatting)       FSMA,    │  │
 │  │                    data)         acceptance/                        CBI,     │  │
 │  │                                  rejection)                         AFM,     │  │
 │  │                                                                     Consob)  │  │
 │  │  External Trades ──▶ Saturn GUI / API / File Upload ──▶ same pipeline       │  │
 │  │                                                                               │  │
 │  │  MiFID Firms:     report to regulator of member's country                    │  │
 │  │  Non-MiFID Firms: report to regulator of instrument's MIC                    │  │
 │  └───────────────────────────────────────────────────────────────────────────────┘  │
 │                                                                                    │
 │  ┌───────────────────────────────────────────────────────────────────────────────┐  │
 │  │  OBOE — Off-Book On-Exchange (Dublin + Oslo)                                 │  │
 │  │                                                                               │  │
 │  │  Trade Declaration ──▶ Check Module ──▶ MDG (publication) ──▶ Members        │  │
 │  │  (via Saturn Web / REST API)             (deferred publication rules)         │  │
 │  │                                                                               │  │
 │  │  Scope: instruments listed on Euronext Dublin & Oslo,                        │  │
 │  │         tradable on Euronext Central Order Book                               │  │
 │  └───────────────────────────────────────────────────────────────────────────────┘  │
 │                                                                                    │
 │  ┌───────────────────────────────────────────────────────────────────────────────┐  │
 │  │  SLC Manager — Short Long Code (MiFID II anonymization)                      │  │
 │  │                                                                               │  │
 │  │  Before:  Order has Member LEI → everyone sees originator                    │  │
 │  │  After:   Order has Short Code 123 → only SATURN maps to LEI               │  │
 │  │                                                                               │  │
 │  │  Entities hidden: Client ID (LEI), human client (National ID),              │  │
 │  │                   AGGR/PNAL flags, Execution Decision maker,                │  │
 │  │                   Investment Decision maker                                  │  │
 │  │                                                                               │  │
 │  │  Transmitted to NCAs via RTS 22/24 regulatory reports                       │  │
 │  └───────────────────────────────────────────────────────────────────────────────┘  │
 │                                                                                    │
 │  ┌───────────────────────────────────────────────────────────────────────────────┐  │
 │  │  Commodities Reporting (MiFID II RTS 21)                                     │  │
 │  │                                                                               │  │
 │  │  Daily: positions per client → ESMA/NCAs                                    │  │
 │  │  Weekly: Commitment of Traders (CoT) → Euronext website                     │  │
 │  │                                                                               │  │
 │  │  Sources: Participants & EMS → Saturn → Check Library                       │  │
 │  │  Reference: Matrix (IDS) + European Instruments Referential                 │  │
 │  └───────────────────────────────────────────────────────────────────────────────┘  │
 │                                                                                    │
 │  Architecture: Kafka Engine + GUI + API + DB                                       │
 │  Communicates with: ESMA, NCAs (AMF, CBI, FCA, AFM, Consob, CNMV, CSSF, FSMA,   │
 │                     CMVM), Euronext Clearing, Borsa Italiana                       │
 └────────────────────────────────────────────────────────────────────────────────────┘
```

---

## Future Expansion: Post-Trade Path

From `Trades paths.drawio` — complete downstream flow after trade execution.

```
 ┌──────────────────────────────────────────────────────────────────────────────────┐
 │                     COMPLETE POST-TRADE PATH  [FUTURE]                           │
 │                                                                                  │
 │  Book/LogCore (trade generated)                                                 │
 │       │                                                                          │
 │       ├──▶ Kafka (recovery + audit trail) ──▶ PE DB (Persistence Engine DB)    │
 │       │                                                                          │
 │       ├──▶ IACA Inside (fragment emission)                                      │
 │       │       │                                                                  │
 │       │       ├──▶ IACA FINISH (chain completion → IA SBE message)             │
 │       │       │       │                                                          │
 │       │       │       ├──▶ IACA COPY (members)  ──▶ Members (DropCopy)         │
 │       │       │       ├──▶ IACA COPY (3rd parties) ──▶ DCG 3P's               │
 │       │       │       ├──▶ IAkafka2DB.TRD ──▶ PE DB (trade persistence)        │
 │       │       │       └──▶ PackFULL / PackTOL ──▶ MDG Emitter ──▶ Members     │
 │       │       │                                                                  │
 │       │       └──▶ IDS (files to IDS) ──▶ Regulators (RTS nn)                  │
 │       │                                                                          │
 │       ├──▶ MktUpdt, FullTradeInfo ──▶ MDG ──▶ Members (market data multicast)  │
 │       │                                                                          │
 │       ├──▶ Trade Fill ──▶ PTB (Post-Trade Box)                                 │
 │       │                       │                                                  │
 │       │                       ├──▶ PTB admin (resilience)                       │
 │       │                       ├──▶ SATURN/OBOE (regulatory reporting)           │
 │       │                       ├──▶ PTB2EC ──▶ Enxt Clearing                    │
 │       │                       ├──▶ EC.POSTTRAD ──▶ Enxt Clearing               │
 │       │                       └──▶ Clearing houses:                             │
 │       │                             EuroCCP, LCH Ltd, SIX Xclear               │
 │       │                                                                          │
 │       └──▶ ACK/NAK ──▶ OEG ──▶ Members (execution reports)                    │
 │                                                                                  │
 │  External entities: DSS, BITA instr., Enxt Legacy instr., SMARTS              │
 │  Risk/Audit: Regulators, Risk/Audit systems                                     │
 └──────────────────────────────────────────────────────────────────────────────────┘
```

---

## Market Data Dissemination

From Trading Manual Section 5.

```
 [FUTURE] Market data types to implement:

 ┌──────────────────────────────────────────────────────────────────┐
 │  1. Market by Orders (MBO)                                      │
 │     Every individual order visible in the book                  │
 │     (currently: not disseminated by EuNEx)                      │
 │                                                                  │
 │  2. Market by Limits (MBL)  ← EuNEx has this (BookUpdateEvent) │
 │     Aggregated qty per price level (BBO + depth)                │
 │     getAsks(n) / getBids(n) already implemented                 │
 │                                                                  │
 │  3. Trades                  ← EuNEx has this (TradeEvent)       │
 │     Each trade with price, qty, timestamp                       │
 │                                                                  │
 │  4. Trading Day Price Summary                                   │
 │     Open, High, Low, Close, Volume, VWAP                        │
 │     (currently: not tracked by EuNEx)                            │
 │                                                                  │
 │  Format: SBE multicast (production) vs in-memory events (EuNEx) │
 └──────────────────────────────────────────────────────────────────┘
```

---

## Implementation Priority Roadmap

```
 Phase 1 (Current) ✓ DONE
 ├── Book with price-time priority matching
 ├── OEGActor → MECoreActor → MDGActor pipeline
 ├── Recovery Cause/Effect with Master/Mirror gating
 ├── IACA fragment chains with completion detection
 └── Simplx shim for single-threaded testing

 Phase 2 — Core Trading Features
 ├── Trading phases (pre-open, uncrossing, continuous, close, TAL)
 ├── Uncrossing algorithm (max execution, min surplus, ref price)
 ├── Price collars (dynamic + static) and reservations
 ├── Additional order types (Stop, Pegged, Mid-Point, Iceberg, M2L)
 ├── Additional validities (GTD, GTC, VFU, VFCU)
 └── Trading groups and instrument configuration

 Phase 3 — Infrastructure
 ├── Real Simplx integration (multi-threaded, multi-core actors)
 ├── Kafka persistence (replace FragmentStore with real Kafka)
 ├── SBE encoding (replace event structs with SBE messages)
 ├── PE DB (Persistence Engine database for order state)
 └── FIX 5.0 SP2 gateway (replace shim OEG with FIX acceptor)

 Phase 4 — Post-Trade & Distribution
 ├── IACA FINISH + IACA COPY (DropCopy to members / 3rd parties)
 ├── MDG multicast SBE feed
 ├── IDS (Intraday Data Service → files to regulators)
 ├── PTB (Post-Trade Box) with trade routing
 └── DCG (DropCopy Gateway for 3rd parties)

 Phase 5 — Regulatory & Clearing
 ├── SATURN ARM (MiFID II RTS 22 trade reporting to NCAs)
 ├── SATURN OBOE (Off-Book On-Exchange for Dublin/Oslo)
 ├── SLC Manager (Short Long Code anonymization)
 ├── Commodities Reporting (RTS 21 position reporting)
 ├── Clearing integration (EuroCCP, LCH Ltd, SIX Xclear)
 └── Enxt Clearing (PTB2EC, EC.POSTTRAD)

 Phase 6 — Scalability & Operations
 ├── Multi-segment support (EQU, ETF, FXI, EQD, IDD, CMO, etc.)
 ├── Partitioning (1 partition = 1 machine, cross-partition routing)
 ├── Full Master/Mirror failover with Kafka replay
 ├── TCS OTC (Trade Capture Service for OTC trades)
 ├── SMARTS market surveillance integration
 └── GUI resilience and operational tooling
```
