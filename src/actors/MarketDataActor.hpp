#pragma once
// ════════════════════════════════════════════════════════════════════
// MarketDataActor — Market Data Publisher
//
// StockEx equivalent: dashboard.py SSE streaming + mdf_simulator.py
//   - Receives trades/snapshots, streams to UI
//
// Optiq equivalent: MDLimitLogicalCoreHandler + MDIMPLogicalCoreHandler
//   - Receives PublishLimitUpdateRequest from Book
//   - Computes limit updates, IMP values
//   - Emits IACA fragments → IA messages for downstream
// ════════════════════════════════════════════════════════════════════

#include "engine/SimplxShim.hpp"
#include "actors/Events.hpp"
#include <vector>
#include <unordered_map>

namespace eunex {

struct MarketDataSnapshot {
    SymbolIndex_t symbolIdx;
    Price_t       lastTradePrice;
    Quantity_t    lastTradeQty;
    Price_t       bestBid;
    Price_t       bestAsk;
    Quantity_t    totalBidQty;
    Quantity_t    totalAskQty;
    uint64_t      tradeCount;
    Timestamp_ns  updateTime;
};

class MarketDataActor : public tredzone::Actor {
public:
    struct Service : tredzone::AsyncService {};

    MarketDataActor();

    void onEvent(const TradeEvent& event);
    void onEvent(const BookUpdateEvent& event);

    const MarketDataSnapshot* getSnapshot(SymbolIndex_t sym) const;
    const std::vector<Trade>& getRecentTrades() const { return recentTrades_; }

private:
    std::unordered_map<SymbolIndex_t, MarketDataSnapshot> snapshots_;
    std::vector<Trade> recentTrades_;
    static constexpr size_t MAX_RECENT_TRADES = 200;
};

} // namespace eunex
