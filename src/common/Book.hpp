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
//
// Mirrors StockEx matcher.py logic but uses sorted std::map for O(log N)
// insert/match instead of Python list.sort() on every match.
//
// In Optiq, each BookActor owns exactly one Book instance.
// The actor guarantees single-threaded access — no locks needed.
//
class Book {
public:
    explicit Book(SymbolIndex_t symbolIdx);

    // Insert a new order and attempt matching. Returns executions via callbacks.
    void newOrder(Order& order, const TradeCallback& onTrade, const ExecCallback& onExec);

    // Cancel resting order by orderId.
    bool cancelOrder(OrderId_t orderId, ExecutionReport& report);

    // Modify resting order (cancel-replace). Price change loses priority.
    bool modifyOrder(OrderId_t orderId, Price_t newPrice, Quantity_t newQty,
                     ExecutionReport& report);

    // ── Queries ────────────────────────────────────────────────────
    struct Level {
        Price_t    price;
        Quantity_t totalQty;
        int        orderCount;
    };

    std::vector<Level> getBids(int depth = 10) const;
    std::vector<Level> getAsks(int depth = 10) const;
    size_t bidCount() const;
    size_t askCount() const;

    SymbolIndex_t symbolIndex() const { return symbolIdx_; }

private:
    SymbolIndex_t symbolIdx_;
    TradeId_t     nextTradeId_ = 1;
    OrderId_t     nextOrderId_ = 1;

    // Price levels: map<price, vector<Order*>>
    // Bids: descending price (std::greater), then FIFO within level
    // Asks: ascending price (std::less), then FIFO within level
    using BidMap = std::map<Price_t, std::vector<Order>, std::greater<Price_t>>;
    using AskMap = std::map<Price_t, std::vector<Order>, std::less<Price_t>>;

    BidMap bids_;
    AskMap asks_;

    // orderId → pointer to resting order for O(1) cancel/modify lookup
    std::unordered_map<OrderId_t, std::pair<Side, Price_t>> orderIndex_;

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
