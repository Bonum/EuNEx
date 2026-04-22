#pragma once
#include "common/Types.hpp"
#include <vector>
#include <map>
#include <unordered_map>
#include <functional>

namespace eunex {

// ── Trade callback type ────────────────────────────────────────────
using TradeCallback = std::function<void(const Trade&)>;
using ExecCallback  = std::function<void(const ExecutionReport&)>;

// ── Price-Time Priority Order Book ─────────────────────────────────
class Book {
public:
    explicit Book(SymbolIndex_t symbolIdx);

    void newOrder(Order& order, const TradeCallback& onTrade, const ExecCallback& onExec);
    bool cancelOrder(OrderId_t orderId, ExecutionReport& report);
    bool modifyOrder(OrderId_t orderId, Price_t newPrice, Quantity_t newQty,
                     ExecutionReport& report);

    // ── Trading phase ─────────────────────────────────────────────
    void setPhase(TradingPhase phase);
    TradingPhase getPhase() const { return phase_; }

    // Indicative Opening Price — computed during PreOpen
    Price_t getIOP() const;

    // Transition PreOpen → Continuous: run uncrossing at IOP
    void uncross(const TradeCallback& onTrade, const ExecCallback& onExec);

    // ── Stop orders ───────────────────────────────────────────────
    void triggerStopOrders(Price_t tradePrice, const TradeCallback& onTrade,
                           const ExecCallback& onExec);
    bool cancelStopOrder(OrderId_t orderId, ExecutionReport& report);

    // ── Queries ────────────────────────────────────────────────────
    struct Level {
        Price_t    price;
        Quantity_t totalQty;
        int        orderCount;
    };

    std::vector<Level> getBids(int depth = 10) const;
    std::vector<Level> getAsks(int depth = 10) const;
    Price_t bestBid() const;
    Price_t bestAsk() const;
    size_t bidCount() const;
    size_t askCount() const;
    size_t stopOrderCount() const { return stopOrders_.size(); }

    SymbolIndex_t symbolIndex() const { return symbolIdx_; }
    Price_t lastTradePrice() const { return lastTradePrice_; }

private:
    SymbolIndex_t symbolIdx_;
    TradeId_t     nextTradeId_ = 1;
    OrderId_t     nextOrderId_ = 1;
    Price_t       lastTradePrice_ = 0;
    TradingPhase  phase_ = TradingPhase::CTS;

    using BidMap = std::map<Price_t, std::vector<Order>, std::greater<Price_t>>;
    using AskMap = std::map<Price_t, std::vector<Order>, std::less<Price_t>>;

    BidMap bids_;
    AskMap asks_;

    std::unordered_map<OrderId_t, std::pair<Side, Price_t>> orderIndex_;

    // Stop orders waiting to be triggered
    std::vector<Order> stopOrders_;

    OrderId_t allocateOrderId() { return nextOrderId_++; }
    TradeId_t allocateTradeId() { return nextTradeId_++; }

    void matchBuy(Order& incoming, const TradeCallback& onTrade, const ExecCallback& onExec);
    void matchSell(Order& incoming, const TradeCallback& onTrade, const ExecCallback& onExec);
    void insertResting(Order& order);
    void removeOrder(OrderId_t orderId, Side side, Price_t price);

    template<typename MapT>
    std::vector<Level> getLevels(const MapT& map, int depth) const;
};

} // namespace eunex
