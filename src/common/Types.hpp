#pragma once
#include <cstdint>
#include <string>
#include <chrono>
#include <limits>

namespace eunex {

// ── Price representation (fixed-point, 8 decimal places) ───────────
// Optiq uses integer-scaled prices to avoid floating-point in the hot path.
// 1 price unit = 10^-8. E.g., price 12.50 EUR = 1'250'000'000.
using Price_t = int64_t;
constexpr int64_t PRICE_SCALE = 100'000'000LL;
constexpr Price_t NULL_PRICE = std::numeric_limits<int64_t>::min();

inline Price_t toFixedPrice(double p) {
    return static_cast<Price_t>(p * PRICE_SCALE);
}
inline double toDouble(Price_t p) {
    return static_cast<double>(p) / PRICE_SCALE;
}

// ── Quantity ────────────────────────────────────────────────────────
using Quantity_t = uint64_t;

// ── Identifiers ────────────────────────────────────────────────────
using OrderId_t     = uint64_t;
using ClOrdId_t     = uint64_t;
using SymbolIndex_t = uint32_t;
using TradeId_t     = uint64_t;
using SessionId_t   = uint16_t;

// ── Enumerations matching Optiq/SBE definitions ────────────────────
enum class Side : uint8_t {
    Buy  = 1,
    Sell = 2
};

enum class OrderType : uint8_t {
    Market = 1,
    Limit  = 2
};

enum class TimeInForce : uint8_t {
    Day = 0,
    GTC = 1,
    IOC = 3,
    FOK = 4
};

enum class OrderStatus : uint8_t {
    New             = 0,
    PartiallyFilled = 1,
    Filled          = 2,
    Cancelled       = 4,
    Rejected        = 8
};

enum class MessageType : uint8_t {
    NewOrder    = 1,
    ModifyOrder = 2,
    CancelOrder = 3,
    MassCancel  = 4
};

// ── Timestamps ─────────────────────────────────────────────────────
using Timestamp_ns = uint64_t;

inline Timestamp_ns nowNs() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();
}

// ── Order structure (compact, cache-friendly) ──────────────────────
// This is the resting order on the book. Incoming order events use
// the Event structs defined in actor headers.
#pragma pack(push, 1)
struct Order {
    OrderId_t     orderId;
    ClOrdId_t     clOrdId;
    SymbolIndex_t symbolIdx;
    Side          side;
    OrderType     ordType;
    TimeInForce   tif;
    Price_t       price;
    Quantity_t    quantity;
    Quantity_t    remainingQty;
    Timestamp_ns  entryTime;
    SessionId_t   sessionId;
    OrderStatus   status;
};
#pragma pack(pop)

// ── Trade structure ────────────────────────────────────────────────
#pragma pack(push, 1)
struct Trade {
    TradeId_t     tradeId;
    SymbolIndex_t symbolIdx;
    Price_t       price;
    Quantity_t    quantity;
    OrderId_t     buyOrderId;
    OrderId_t     sellOrderId;
    ClOrdId_t     buyClOrdId;
    ClOrdId_t     sellClOrdId;
    Timestamp_ns  matchTime;
};
#pragma pack(pop)

// ── Execution report ───────────────────────────────────────────
struct ExecutionReport {
    OrderId_t    orderId;
    ClOrdId_t    clOrdId;
    OrderStatus  status;
    Quantity_t   filledQty;
    Quantity_t   remainingQty;
    Price_t      lastPrice;
    Quantity_t   lastQty;
    TradeId_t    tradeId;
};

// ── Symbol definition ──────────────────────────────────────────────
struct SymbolDef {
    SymbolIndex_t index;
    char          isin[12];
    char          mnemonic[8];
    Price_t       tickSize;
    Quantity_t    lotSize;
};

} // namespace eunex
