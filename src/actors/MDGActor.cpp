#include "actors/MDGActor.hpp"

namespace eunex {

MDGActor::MDGActor() {
    registerEventHandler<TradeEvent>(*this);
    registerEventHandler<BookUpdateEvent>(*this);
}

void MDGActor::onEvent(const TradeEvent& event) {
    const auto& t = event.trade;
    auto& snap = snapshots_[t.symbolIdx];
    snap.symbolIdx      = t.symbolIdx;
    snap.lastTradePrice  = t.price;
    snap.lastTradeQty    = t.quantity;
    snap.tradeCount++;
    snap.updateTime      = nowNs();

    recentTrades_.push_back(t);
    if (recentTrades_.size() > MAX_RECENT_TRADES) {
        recentTrades_.erase(recentTrades_.begin());
    }
}

void MDGActor::onEvent(const BookUpdateEvent& event) {
    auto& snap = snapshots_[event.symbolIdx];
    snap.symbolIdx = event.symbolIdx;
    snap.updateTime = nowNs();

    if (event.bidDepth > 0) {
        snap.bestBid = event.bids[0].price;
        snap.totalBidQty = 0;
        for (int i = 0; i < event.bidDepth; ++i)
            snap.totalBidQty += event.bids[i].qty;
    }

    if (event.askDepth > 0) {
        snap.bestAsk = event.asks[0].price;
        snap.totalAskQty = 0;
        for (int i = 0; i < event.askDepth; ++i)
            snap.totalAskQty += event.asks[i].qty;
    }
}

const MarketDataSnapshot* MDGActor::getSnapshot(SymbolIndex_t sym) const {
    auto it = snapshots_.find(sym);
    return it != snapshots_.end() ? &it->second : nullptr;
}

} // namespace eunex
