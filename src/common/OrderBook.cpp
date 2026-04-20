#include "common/OrderBook.hpp"
#include <algorithm>

namespace eunex {

OrderBook::OrderBook(SymbolIndex_t symbolIdx)
    : symbolIdx_(symbolIdx) {}

// ── New Order Entry ────────────────────────────────────────────────

void OrderBook::newOrder(Order& order, const TradeCallback& onTrade,
                         const ExecCallback& onExec) {
    order.orderId = allocateOrderId();
    order.remainingQty = order.quantity;
    order.status = OrderStatus::New;
    order.entryTime = nowNs();

    // FOK: check if full fill is possible before matching
    if (order.tif == TimeInForce::FOK) {
        Quantity_t available = 0;
        if (order.side == Side::Buy) {
            for (auto& [price, orders] : asks_) {
                if (order.ordType != OrderType::Market && price > order.price)
                    break;
                for (auto& resting : orders)
                    available += resting.remainingQty;
                if (available >= order.quantity) break;
            }
        } else {
            for (auto& [price, orders] : bids_) {
                if (order.ordType != OrderType::Market && price < order.price)
                    break;
                for (auto& resting : orders)
                    available += resting.remainingQty;
                if (available >= order.quantity) break;
            }
        }
        if (available < order.quantity) {
            order.status = OrderStatus::Rejected;
            ExecutionReport rpt{order.orderId, order.clOrdId, OrderStatus::Rejected,
                                0, order.quantity, 0, 0, 0};
            onExec(rpt);
            return;
        }
    }

    if (order.side == Side::Buy) {
        matchBuy(order, onTrade, onExec);
    } else {
        matchSell(order, onTrade, onExec);
    }

    // Resting logic: Market and IOC never rest on book
    if (order.remainingQty > 0) {
        if (order.ordType == OrderType::Market || order.tif == TimeInForce::IOC) {
            order.status = OrderStatus::Cancelled;
            ExecutionReport rpt{order.orderId, order.clOrdId, OrderStatus::Cancelled,
                                order.quantity - order.remainingQty, 0, 0, 0, 0};
            onExec(rpt);
        } else {
            order.status = (order.remainingQty < order.quantity)
                ? OrderStatus::PartiallyFilled : OrderStatus::New;
            insertResting(order);
            ExecutionReport rpt{order.orderId, order.clOrdId, order.status,
                                order.quantity - order.remainingQty,
                                order.remainingQty, 0, 0, 0};
            onExec(rpt);
        }
    } else {
        order.status = OrderStatus::Filled;
        ExecutionReport rpt{order.orderId, order.clOrdId, OrderStatus::Filled,
                            order.quantity, 0, 0, 0, 0};
        onExec(rpt);
    }
}

// ── Matching: Buy against Asks ─────────────────────────────────────

void OrderBook::matchBuy(Order& incoming, const TradeCallback& onTrade,
                         const ExecCallback& onExec) {
    auto it = asks_.begin();
    while (incoming.remainingQty > 0 && it != asks_.end()) {
        Price_t askPrice = it->first;

        if (incoming.ordType != OrderType::Market && askPrice > incoming.price)
            break;

        auto& level = it->second;
        auto orderIt = level.begin();
        while (incoming.remainingQty > 0 && orderIt != level.end()) {
            Quantity_t traded = std::min(incoming.remainingQty, orderIt->remainingQty);
            Price_t tradePrice = askPrice;

            Trade trade{};
            trade.tradeId     = allocateTradeId();
            trade.symbolIdx   = symbolIdx_;
            trade.price       = tradePrice;
            trade.quantity    = traded;
            trade.buyOrderId  = incoming.orderId;
            trade.sellOrderId = orderIt->orderId;
            trade.buyClOrdId  = incoming.clOrdId;
            trade.sellClOrdId = orderIt->clOrdId;
            trade.matchTime   = nowNs();
            onTrade(trade);

            incoming.remainingQty -= traded;
            orderIt->remainingQty -= traded;

            // Execution report for the resting sell
            ExecutionReport restingRpt{orderIt->orderId, orderIt->clOrdId,
                orderIt->remainingQty == 0 ? OrderStatus::Filled : OrderStatus::PartiallyFilled,
                orderIt->quantity - orderIt->remainingQty,
                orderIt->remainingQty, tradePrice, traded, trade.tradeId};
            onExec(restingRpt);

            if (orderIt->remainingQty == 0) {
                orderIndex_.erase(orderIt->orderId);
                orderIt = level.erase(orderIt);
            } else {
                ++orderIt;
            }
        }

        if (level.empty()) {
            it = asks_.erase(it);
        } else {
            ++it;
        }
    }
}

// ── Matching: Sell against Bids ────────────────────────────────────

void OrderBook::matchSell(Order& incoming, const TradeCallback& onTrade,
                          const ExecCallback& onExec) {
    auto it = bids_.begin();
    while (incoming.remainingQty > 0 && it != bids_.end()) {
        Price_t bidPrice = it->first;

        if (incoming.ordType != OrderType::Market && bidPrice < incoming.price)
            break;

        auto& level = it->second;
        auto orderIt = level.begin();
        while (incoming.remainingQty > 0 && orderIt != level.end()) {
            Quantity_t traded = std::min(incoming.remainingQty, orderIt->remainingQty);
            Price_t tradePrice = bidPrice;

            Trade trade{};
            trade.tradeId     = allocateTradeId();
            trade.symbolIdx   = symbolIdx_;
            trade.price       = tradePrice;
            trade.quantity    = traded;
            trade.buyOrderId  = orderIt->orderId;
            trade.sellOrderId = incoming.orderId;
            trade.buyClOrdId  = orderIt->clOrdId;
            trade.sellClOrdId = incoming.clOrdId;
            trade.matchTime   = nowNs();
            onTrade(trade);

            incoming.remainingQty -= traded;
            orderIt->remainingQty -= traded;

            ExecutionReport restingRpt{orderIt->orderId, orderIt->clOrdId,
                orderIt->remainingQty == 0 ? OrderStatus::Filled : OrderStatus::PartiallyFilled,
                orderIt->quantity - orderIt->remainingQty,
                orderIt->remainingQty, tradePrice, traded, trade.tradeId};
            onExec(restingRpt);

            if (orderIt->remainingQty == 0) {
                orderIndex_.erase(orderIt->orderId);
                orderIt = level.erase(orderIt);
            } else {
                ++orderIt;
            }
        }

        if (level.empty()) {
            it = bids_.erase(it);
        } else {
            ++it;
        }
    }
}

// ── Cancel ─────────────────────────────────────────────────────────

bool OrderBook::cancelOrder(OrderId_t orderId, ExecutionReport& report) {
    auto it = orderIndex_.find(orderId);
    if (it == orderIndex_.end())
        return false;

    auto [side, price] = it->second;

    if (side == Side::Buy) {
        auto levelIt = bids_.find(price);
        if (levelIt != bids_.end()) {
            auto& vec = levelIt->second;
            for (auto oit = vec.begin(); oit != vec.end(); ++oit) {
                if (oit->orderId == orderId) {
                    report = {orderId, oit->clOrdId, OrderStatus::Cancelled,
                              oit->quantity - oit->remainingQty, 0, 0, 0, 0};
                    vec.erase(oit);
                    if (vec.empty()) bids_.erase(levelIt);
                    orderIndex_.erase(it);
                    return true;
                }
            }
        }
    } else {
        auto levelIt = asks_.find(price);
        if (levelIt != asks_.end()) {
            auto& vec = levelIt->second;
            for (auto oit = vec.begin(); oit != vec.end(); ++oit) {
                if (oit->orderId == orderId) {
                    report = {orderId, oit->clOrdId, OrderStatus::Cancelled,
                              oit->quantity - oit->remainingQty, 0, 0, 0, 0};
                    vec.erase(oit);
                    if (vec.empty()) asks_.erase(levelIt);
                    orderIndex_.erase(it);
                    return true;
                }
            }
        }
    }
    return false;
}

// ── Modify (Cancel-Replace) ────────────────────────────────────────

bool OrderBook::modifyOrder(OrderId_t orderId, Price_t newPrice, Quantity_t newQty,
                            ExecutionReport& report) {
    ExecutionReport cancelRpt{};
    if (!cancelOrder(orderId, cancelRpt))
        return false;

    Order replacement{};
    replacement.clOrdId    = cancelRpt.clOrdId;
    replacement.symbolIdx  = symbolIdx_;
    replacement.side       = (bids_.count(cancelRpt.lastPrice)) ? Side::Buy : Side::Sell;
    replacement.ordType    = OrderType::Limit;
    replacement.tif        = TimeInForce::Day;
    replacement.price      = newPrice;
    replacement.quantity   = newQty;
    replacement.remainingQty = newQty;
    replacement.orderId    = allocateOrderId();
    replacement.entryTime  = nowNs();
    replacement.status     = OrderStatus::New;

    insertResting(replacement);
    report = {replacement.orderId, replacement.clOrdId, OrderStatus::New,
              0, newQty, 0, 0, 0};
    return true;
}

// ── Insert resting order ───────────────────────────────────────────

void OrderBook::insertResting(Order& order) {
    orderIndex_[order.orderId] = {order.side, order.price};
    if (order.side == Side::Buy) {
        bids_[order.price].push_back(order);
    } else {
        asks_[order.price].push_back(order);
    }
}

void OrderBook::removeOrder(OrderId_t orderId, Side side, Price_t price) {
    orderIndex_.erase(orderId);
    if (side == Side::Buy) {
        auto it = bids_.find(price);
        if (it != bids_.end()) {
            auto& vec = it->second;
            vec.erase(std::remove_if(vec.begin(), vec.end(),
                [orderId](const Order& o) { return o.orderId == orderId; }), vec.end());
            if (vec.empty()) bids_.erase(it);
        }
    } else {
        auto it = asks_.find(price);
        if (it != asks_.end()) {
            auto& vec = it->second;
            vec.erase(std::remove_if(vec.begin(), vec.end(),
                [orderId](const Order& o) { return o.orderId == orderId; }), vec.end());
            if (vec.empty()) asks_.erase(it);
        }
    }
}

// ── Query helpers ──────────────────────────────────────────────────

template<typename MapT>
std::vector<OrderBook::Level> OrderBook::getLevels(const MapT& map, int depth) const {
    std::vector<Level> result;
    result.reserve(depth);
    int count = 0;
    for (auto& [price, orders] : map) {
        if (count >= depth) break;
        Quantity_t totalQty = 0;
        for (auto& o : orders) totalQty += o.remainingQty;
        result.push_back({price, totalQty, static_cast<int>(orders.size())});
        ++count;
    }
    return result;
}

std::vector<OrderBook::Level> OrderBook::getBids(int depth) const {
    return getLevels(bids_, depth);
}

std::vector<OrderBook::Level> OrderBook::getAsks(int depth) const {
    return getLevels(asks_, depth);
}

size_t OrderBook::bidCount() const {
    size_t count = 0;
    for (auto& [_, orders] : bids_) count += orders.size();
    return count;
}

size_t OrderBook::askCount() const {
    size_t count = 0;
    for (auto& [_, orders] : asks_) count += orders.size();
    return count;
}

} // namespace eunex
