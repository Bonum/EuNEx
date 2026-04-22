#include "common/Book.hpp"
#include <algorithm>
#include <cmath>

namespace eunex {

Book::Book(SymbolIndex_t symbolIdx)
    : symbolIdx_(symbolIdx) {}

// ── Phase management ──────────────────────────────────────────────

void Book::setPhase(TradingPhase phase) {
    phase_ = phase;
}

// ── IOP: Indicative Opening Price ─────────────────────────────────
// Find the price that maximizes executable volume.
// Tiebreakers: minimum surplus, then higher price if buy surplus.

Price_t Book::getIOP() const {
    if (bids_.empty() || asks_.empty()) return 0;

    // Collect all candidate prices from both sides
    std::vector<Price_t> prices;
    for (auto& [p, _] : bids_) prices.push_back(p);
    for (auto& [p, _] : asks_) prices.push_back(p);
    std::sort(prices.begin(), prices.end());
    prices.erase(std::unique(prices.begin(), prices.end()), prices.end());

    Price_t bestPrice = 0;
    Quantity_t bestVolume = 0;
    Quantity_t bestSurplus = UINT64_MAX;
    bool bestBuySurplus = false;

    for (Price_t p : prices) {
        // Cumulative buy qty at price >= p
        Quantity_t cumBuy = 0;
        for (auto& [bidPrice, orders] : bids_) {
            if (bidPrice >= p) {
                for (auto& o : orders) cumBuy += o.remainingQty;
            }
        }
        // Cumulative sell qty at price <= p
        Quantity_t cumSell = 0;
        for (auto& [askPrice, orders] : asks_) {
            if (askPrice <= p) {
                for (auto& o : orders) cumSell += o.remainingQty;
            }
        }

        Quantity_t volume = std::min(cumBuy, cumSell);
        Quantity_t surplus = (cumBuy > cumSell) ? (cumBuy - cumSell) : (cumSell - cumBuy);
        bool buySurplus = cumBuy > cumSell;

        if (volume > bestVolume ||
            (volume == bestVolume && surplus < bestSurplus) ||
            (volume == bestVolume && surplus == bestSurplus && buySurplus && p > bestPrice)) {
            bestPrice = p;
            bestVolume = volume;
            bestSurplus = surplus;
            bestBuySurplus = buySurplus;
        }
    }

    return bestVolume > 0 ? bestPrice : 0;
}

// ── Uncrossing: execute accumulated orders at IOP ─────────────────

void Book::uncross(const TradeCallback& onTrade, const ExecCallback& onExec) {
    Price_t iop = getIOP();
    if (iop == 0) return;

    Quantity_t remainingVolume = UINT64_MAX;

    // Match bids >= IOP against asks <= IOP at the IOP price
    // Process asks in price-time order
    auto askIt = asks_.begin();
    while (askIt != asks_.end() && askIt->first <= iop && remainingVolume > 0) {
        auto& level = askIt->second;
        auto orderIt = level.begin();
        while (orderIt != level.end() && remainingVolume > 0) {
            // Find a matching bid
            auto bidIt = bids_.begin();
            while (bidIt != bids_.end() && bidIt->first >= iop && orderIt->remainingQty > 0) {
                auto& bidLevel = bidIt->second;
                auto bidOrderIt = bidLevel.begin();
                while (bidOrderIt != bidLevel.end() && orderIt->remainingQty > 0) {
                    Quantity_t traded = std::min(orderIt->remainingQty, bidOrderIt->remainingQty);

                    Trade trade{};
                    trade.tradeId       = allocateTradeId();
                    trade.symbolIdx     = symbolIdx_;
                    trade.price         = iop;
                    trade.quantity      = traded;
                    trade.buyOrderId    = bidOrderIt->orderId;
                    trade.sellOrderId   = orderIt->orderId;
                    trade.buyClOrdId    = bidOrderIt->clOrdId;
                    trade.sellClOrdId   = orderIt->clOrdId;
                    trade.buySessionId  = bidOrderIt->sessionId;
                    trade.sellSessionId = orderIt->sessionId;
                    trade.matchTime     = nowNs();
                    onTrade(trade);
                    lastTradePrice_ = iop;

                    orderIt->remainingQty -= traded;
                    bidOrderIt->remainingQty -= traded;

                    ExecutionReport bidRpt{bidOrderIt->orderId, bidOrderIt->clOrdId,
                        bidOrderIt->remainingQty == 0 ? OrderStatus::Filled : OrderStatus::PartiallyFilled,
                        bidOrderIt->quantity - bidOrderIt->remainingQty,
                        bidOrderIt->remainingQty, iop, traded, trade.tradeId};
                    onExec(bidRpt);

                    ExecutionReport askRpt{orderIt->orderId, orderIt->clOrdId,
                        orderIt->remainingQty == 0 ? OrderStatus::Filled : OrderStatus::PartiallyFilled,
                        orderIt->quantity - orderIt->remainingQty,
                        orderIt->remainingQty, iop, traded, trade.tradeId};
                    onExec(askRpt);

                    if (bidOrderIt->remainingQty == 0) {
                        orderIndex_.erase(bidOrderIt->orderId);
                        bidOrderIt = bidLevel.erase(bidOrderIt);
                    } else {
                        ++bidOrderIt;
                    }
                }
                if (bidLevel.empty()) {
                    bidIt = bids_.erase(bidIt);
                } else {
                    ++bidIt;
                }
            }

            if (orderIt->remainingQty == 0) {
                orderIndex_.erase(orderIt->orderId);
                orderIt = level.erase(orderIt);
            } else {
                ++orderIt;
            }
        }
        if (level.empty()) {
            askIt = asks_.erase(askIt);
        } else {
            ++askIt;
        }
    }
}

// ── New Order Entry ────────────────────────────────────────────────

void Book::newOrder(Order& order, const TradeCallback& onTrade,
                         const ExecCallback& onExec) {
    order.orderId = allocateOrderId();
    order.remainingQty = order.quantity;
    order.status = OrderStatus::New;
    order.entryTime = nowNs();

    // Stop orders: park without matching, wait for trigger
    if (order.ordType == OrderType::StopMarket || order.ordType == OrderType::StopLimit) {
        stopOrders_.push_back(order);
        ExecutionReport rpt{order.orderId, order.clOrdId, OrderStatus::New,
                            0, order.quantity, 0, 0, 0};
        onExec(rpt);
        return;
    }

    // PreOpen / Opening: accumulate orders on book without matching
    if (phase_ == TradingPhase::PreOpen || phase_ == TradingPhase::Opening) {
        insertResting(order);
        ExecutionReport rpt{order.orderId, order.clOrdId, OrderStatus::New,
                            0, order.quantity, 0, 0, 0};
        onExec(rpt);
        return;
    }

    // Closed: reject all orders
    if (phase_ == TradingPhase::Closed) {
        order.status = OrderStatus::Rejected;
        ExecutionReport rpt{order.orderId, order.clOrdId, OrderStatus::Rejected,
                            0, order.quantity, 0, 0, 0};
        onExec(rpt);
        return;
    }

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

// ── Stop order trigger ────────────────────────────────────────────
// Buy stop: triggered when tradePrice >= stopPrice (price rising)
// Sell stop: triggered when tradePrice <= stopPrice (price falling)

void Book::triggerStopOrders(Price_t tradePrice, const TradeCallback& onTrade,
                              const ExecCallback& onExec) {
    std::vector<Order> remaining;
    std::vector<Order> triggered;

    for (auto& stop : stopOrders_) {
        bool trigger = false;
        if (stop.side == Side::Buy && tradePrice >= stop.stopPrice)
            trigger = true;
        else if (stop.side == Side::Sell && tradePrice <= stop.stopPrice)
            trigger = true;

        if (trigger) {
            triggered.push_back(stop);
        } else {
            remaining.push_back(stop);
        }
    }

    stopOrders_ = std::move(remaining);

    for (auto& stop : triggered) {
        Order converted{};
        converted.clOrdId   = stop.clOrdId;
        converted.symbolIdx = stop.symbolIdx;
        converted.side      = stop.side;
        converted.tif       = stop.tif;
        converted.quantity   = stop.remainingQty;
        converted.sessionId  = stop.sessionId;
        converted.stopPrice  = 0;

        if (stop.ordType == OrderType::StopMarket) {
            converted.ordType = OrderType::Market;
            converted.price   = 0;
        } else {
            converted.ordType = OrderType::Limit;
            converted.price   = stop.price;
        }

        newOrder(converted, onTrade, onExec);
    }
}

// ── Cancel stop order ─────────────────────────────────────────────

bool Book::cancelStopOrder(OrderId_t orderId, ExecutionReport& report) {
    for (auto it = stopOrders_.begin(); it != stopOrders_.end(); ++it) {
        if (it->orderId == orderId) {
            report = {orderId, it->clOrdId, OrderStatus::Cancelled,
                      0, 0, 0, 0, 0};
            stopOrders_.erase(it);
            return true;
        }
    }
    return false;
}

// ── Matching: Buy against Asks ─────────────────────────────────────

void Book::matchBuy(Order& incoming, const TradeCallback& onTrade,
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
            trade.buySessionId  = incoming.sessionId;
            trade.sellSessionId = orderIt->sessionId;
            trade.matchTime   = nowNs();
            onTrade(trade);
            lastTradePrice_ = tradePrice;

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
            it = asks_.erase(it);
        } else {
            ++it;
        }
    }
}

// ── Matching: Sell against Bids ────────────────────────────────────

void Book::matchSell(Order& incoming, const TradeCallback& onTrade,
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
            trade.buySessionId  = orderIt->sessionId;
            trade.sellSessionId = incoming.sessionId;
            trade.matchTime   = nowNs();
            onTrade(trade);
            lastTradePrice_ = tradePrice;

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

bool Book::cancelOrder(OrderId_t orderId, ExecutionReport& report) {
    // Check stop orders first
    if (cancelStopOrder(orderId, report))
        return true;

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

bool Book::modifyOrder(OrderId_t orderId, Price_t newPrice, Quantity_t newQty,
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
    replacement.stopPrice  = 0;

    insertResting(replacement);
    report = {replacement.orderId, replacement.clOrdId, OrderStatus::New,
              0, newQty, 0, 0, 0};
    return true;
}

// ── Insert resting order ───────────────────────────────────────────

void Book::insertResting(Order& order) {
    orderIndex_[order.orderId] = {order.side, order.price};
    if (order.side == Side::Buy) {
        bids_[order.price].push_back(order);
    } else {
        asks_[order.price].push_back(order);
    }
}

void Book::removeOrder(OrderId_t orderId, Side side, Price_t price) {
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

Price_t Book::bestBid() const {
    return bids_.empty() ? 0 : bids_.begin()->first;
}

Price_t Book::bestAsk() const {
    return asks_.empty() ? 0 : asks_.begin()->first;
}

template<typename MapT>
std::vector<Book::Level> Book::getLevels(const MapT& map, int depth) const {
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

std::vector<Book::Level> Book::getBids(int depth) const {
    return getLevels(bids_, depth);
}

std::vector<Book::Level> Book::getAsks(int depth) const {
    return getLevels(asks_, depth);
}

size_t Book::bidCount() const {
    size_t count = 0;
    for (auto& [_, orders] : bids_) count += orders.size();
    return count;
}

size_t Book::askCount() const {
    size_t count = 0;
    for (auto& [_, orders] : asks_) count += orders.size();
    return count;
}

} // namespace eunex
