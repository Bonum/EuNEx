#pragma once
// ════════════════════════════════════════════════════════════════════
// Event definitions for inter-actor communication
//
// Maps to StockEx Kafka messages but as typed C++ structs flowing
// through Simplx Event::Pipe instead of JSON over Kafka.
//
// Optiq equivalent: these correspond to the internal events between
// OEActor, LogicalCoreActor, BookActor, and MD publishers.
// ════════════════════════════════════════════════════════════════════

#include "engine/SimplxShim.hpp"
#include "common/Types.hpp"
#include <cstring>

namespace eunex {

// ── NewOrder event (OE Gateway → OrderBook Actor) ──────────────────
// Equivalent to StockEx: Kafka 'orders' topic message with type=new
// Optiq equivalent: FastNewLimitEvent / FastNewMarketEvent
struct NewOrderEvent : tredzone::Actor::Event {
    ClOrdId_t     clOrdId;
    SymbolIndex_t symbolIdx;
    Side          side;
    OrderType     ordType;
    TimeInForce   tif;
    Price_t       price;
    Quantity_t    quantity;
    SessionId_t   sessionId;
    Price_t       stopPrice;

    NewOrderEvent() : stopPrice(0) {}
    NewOrderEvent(ClOrdId_t cl, SymbolIndex_t sym, Side s, OrderType ot,
                  TimeInForce t, Price_t px, Quantity_t qty, SessionId_t sess,
                  Price_t stop = 0)
        : clOrdId(cl), symbolIdx(sym), side(s), ordType(ot),
          tif(t), price(px), quantity(qty), sessionId(sess),
          stopPrice(stop) {}
};

// ── CancelOrder event ──────────────────────────────────────────────
// Equivalent to StockEx: Kafka message with type=cancel
// Optiq equivalent: CancelOrderData
struct CancelOrderEvent : tredzone::Actor::Event {
    OrderId_t     orderId;
    ClOrdId_t     origClOrdId;
    SymbolIndex_t symbolIdx;
    SessionId_t   sessionId;

    CancelOrderEvent() = default;
    CancelOrderEvent(OrderId_t oid, ClOrdId_t cl, SymbolIndex_t sym, SessionId_t sess)
        : orderId(oid), origClOrdId(cl), symbolIdx(sym), sessionId(sess) {}
};

// ── ModifyOrder event ──────────────────────────────────────────────
// Equivalent to StockEx: Kafka message with type=amend
// Optiq equivalent: ModifyOrderData
struct ModifyOrderEvent : tredzone::Actor::Event {
    OrderId_t     orderId;
    ClOrdId_t     origClOrdId;
    SymbolIndex_t symbolIdx;
    Price_t       newPrice;
    Quantity_t    newQuantity;
    SessionId_t   sessionId;

    ModifyOrderEvent() = default;
    ModifyOrderEvent(OrderId_t oid, ClOrdId_t cl, SymbolIndex_t sym,
                     Price_t px, Quantity_t qty, SessionId_t sess)
        : orderId(oid), origClOrdId(cl), symbolIdx(sym),
          newPrice(px), newQuantity(qty), sessionId(sess) {}
};

// ── ExecutionReport event (OrderBook Actor → OE Gateway) ───────────
// Optiq equivalent: Ack sent back to OE frontal via sendToOEEffect
struct ExecReportEvent : tredzone::Actor::Event {
    OrderId_t   orderId;
    ClOrdId_t   clOrdId;
    OrderStatus status;
    Quantity_t  filledQty;
    Quantity_t  remainingQty;
    Price_t     lastPrice;
    Quantity_t  lastQty;
    TradeId_t   tradeId;
    SessionId_t sessionId;

    ExecReportEvent() = default;
    ExecReportEvent(const ExecutionReport& rpt, SessionId_t sess)
        : orderId(rpt.orderId), clOrdId(rpt.clOrdId), status(rpt.status),
          filledQty(rpt.filledQty), remainingQty(rpt.remainingQty),
          lastPrice(rpt.lastPrice), lastQty(rpt.lastQty),
          tradeId(rpt.tradeId), sessionId(sess) {}
    ExecReportEvent(const ExecReportEvent& other)
        : orderId(other.orderId), clOrdId(other.clOrdId), status(other.status),
          filledQty(other.filledQty), remainingQty(other.remainingQty),
          lastPrice(other.lastPrice), lastQty(other.lastQty),
          tradeId(other.tradeId), sessionId(other.sessionId) {}
};

// ── Trade event (OrderBook Actor → MarketData Actor) ───────────────
// Optiq equivalent: trade fragment sent to MDLimit/MDIMP via IACA chain
struct TradeEvent : tredzone::Actor::Event {
    Trade trade;

    TradeEvent() = default;
    explicit TradeEvent(const Trade& t) : trade(t) {}
};

// ── Market Data snapshot event (OrderBook → MarketData Actor) ──────
// Optiq equivalent: PublishLimitUpdateRequest to MDLimitLogicalCoreHandler
struct BookUpdateEvent : tredzone::Actor::Event {
    SymbolIndex_t symbolIdx;
    struct Level {
        Price_t    price;
        Quantity_t qty;
    };
    Level bids[10];
    Level asks[10];
    int   bidDepth;
    int   askDepth;

    BookUpdateEvent() : symbolIdx(0), bidDepth(0), askDepth(0) {
        std::memset(bids, 0, sizeof(bids));
        std::memset(asks, 0, sizeof(asks));
    }
};

// ── Recovery fragment event (any actor → PersistenceAgent) ─────────
// Optiq equivalent: WriteRecoveryFragmentEvent to CoreAgentActor
struct RecoveryFragmentEvent : tredzone::Actor::Event {
    uint8_t  persistenceId;
    uint16_t originId;
    uint32_t originKey;
    uint64_t sequenceNumber;
    uint8_t  payload[4096];
    size_t   payloadSize;

    RecoveryFragmentEvent() : persistenceId(0), originId(0), originKey(0),
        sequenceNumber(0), payloadSize(0) {}
};

} // namespace eunex
