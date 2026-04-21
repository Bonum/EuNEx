#include "actors/AITraderActor.hpp"
#include <algorithm>
#include <numeric>
#include <cstring>
#include <iostream>

namespace eunex {

AITraderActor::AITraderActor(const tredzone::ActorId& oeGatewayId,
                              const std::vector<SymbolIndex_t>& symbols)
    : oePipe_(*this, oeGatewayId)
    , symbols_(symbols)
    , rng_(std::random_device{}())
{
    registerEventHandler<BookUpdateEvent>(*this);
    registerEventHandler<TradeEvent>(*this);
    registerEventHandler<ExecReportEvent>(*this);
    initMembers();
    registerCallback(*this);
}

void AITraderActor::initMembers() {
    Strategy strategies[] = {Strategy::Momentum, Strategy::MeanReversion, Strategy::Random};

    for (int i = 0; i < NUM_MEMBERS; ++i) {
        AITraderMember m{};
        m.memberId = static_cast<MemberId_t>(i + 1);
        m.sessionId = static_cast<SessionId_t>(200 + i);
        std::snprintf(m.name, sizeof(m.name), "MBR%02d", i + 1);
        m.strategy = strategies[i % 3];
        m.orderCount = 0;
        members_.push_back(m);
    }
}

void AITraderActor::onEvent(const BookUpdateEvent& event) {
    BBO bbo{};
    if (event.bidDepth > 0) bbo.bestBid = event.bids[0].price;
    if (event.askDepth > 0) bbo.bestAsk = event.asks[0].price;
    bbos_[event.symbolIdx] = bbo;
}

void AITraderActor::onEvent(const TradeEvent& event) {
    auto& history = priceHistory_[event.trade.symbolIdx];
    history.push_back(event.trade.price);
    if (history.size() > MAX_PRICE_HISTORY) {
        history.erase(history.begin());
    }
}

void AITraderActor::onEvent(const ExecReportEvent&) {
    // Could track fills per member; not needed for basic trading
}

void AITraderActor::onCallback() {
    tradeRound();
    registerCallback(*this);
}

void AITraderActor::tradeRound() {
    if (symbols_.empty()) return;

    for (auto& member : members_) {
        std::uniform_int_distribution<size_t> symDist(0, symbols_.size() - 1);
        SymbolIndex_t symIdx = symbols_[symDist(rng_)];

        auto bboIt = bbos_.find(symIdx);
        if (bboIt == bbos_.end() || (bboIt->second.bestBid == 0 && bboIt->second.bestAsk == 0)) {
            submitOrder(member, symIdx);
            continue;
        }

        const BBO& bbo = bboIt->second;
        auto histIt = priceHistory_.find(symIdx);
        std::vector<Price_t> emptyHist;
        const auto& history = (histIt != priceHistory_.end()) ? histIt->second : emptyHist;

        switch (member.strategy) {
            case Strategy::Momentum:
                strategyMomentum(member, symIdx, bbo, history);
                break;
            case Strategy::MeanReversion:
                strategyMeanReversion(member, symIdx, bbo, history);
                break;
            case Strategy::Random:
                strategyRandom(member, symIdx, bbo);
                break;
        }
    }
}

void AITraderActor::submitOrder(const AITraderMember& member, SymbolIndex_t symIdx) {
    std::uniform_int_distribution<int> sideDist(0, 1);
    std::uniform_int_distribution<int> priceDist(100, 200);
    std::uniform_int_distribution<int> qtyDist(10, 100);

    Side side = sideDist(rng_) ? Side::Buy : Side::Sell;
    Price_t price = toFixedPrice(priceDist(rng_) * 1.0);
    Quantity_t qty = qtyDist(rng_);
    ClOrdId_t clOrdId = nextClOrdId_++;

    oePipe_.push<NewOrderEvent>(clOrdId, symIdx, side, OrderType::Limit,
                                 TimeInForce::Day, price, qty, member.sessionId);
}

void AITraderActor::strategyMomentum(const AITraderMember& member, SymbolIndex_t symIdx,
                                      const BBO& bbo, const std::vector<Price_t>& history) {
    if (history.size() < 3) {
        submitOrder(member, symIdx);
        return;
    }

    Price_t recent = history.back();
    Price_t older = history[history.size() - 3];

    Side side = (recent > older) ? Side::Buy : Side::Sell;
    Price_t midPrice = (bbo.bestBid + bbo.bestAsk) / 2;
    if (midPrice == 0) midPrice = recent;

    std::uniform_int_distribution<int> spreadDist(-2, 2);
    Price_t tickOffset = spreadDist(rng_) * (PRICE_SCALE / 100);
    Price_t price = midPrice + tickOffset;
    if (price <= 0) price = PRICE_SCALE;

    std::uniform_int_distribution<int> qtyDist(10, 50);
    Quantity_t qty = qtyDist(rng_);
    ClOrdId_t clOrdId = nextClOrdId_++;

    oePipe_.push<NewOrderEvent>(clOrdId, symIdx, side, OrderType::Limit,
                                 TimeInForce::Day, price, qty, member.sessionId);
}

void AITraderActor::strategyMeanReversion(const AITraderMember& member, SymbolIndex_t symIdx,
                                           const BBO& bbo, const std::vector<Price_t>& history) {
    if (history.size() < 5) {
        submitOrder(member, symIdx);
        return;
    }

    int64_t sum = 0;
    for (auto p : history) sum += p;
    Price_t mean = static_cast<Price_t>(sum / static_cast<int64_t>(history.size()));
    Price_t current = history.back();

    Side side = (current > mean) ? Side::Sell : Side::Buy;

    Price_t price = mean;
    std::uniform_int_distribution<int> qtyDist(10, 50);
    Quantity_t qty = qtyDist(rng_);
    ClOrdId_t clOrdId = nextClOrdId_++;

    oePipe_.push<NewOrderEvent>(clOrdId, symIdx, side, OrderType::Limit,
                                 TimeInForce::Day, price, qty, member.sessionId);
}

void AITraderActor::strategyRandom(const AITraderMember& member, SymbolIndex_t symIdx,
                                    const BBO& bbo) {
    std::uniform_int_distribution<int> sideDist(0, 1);
    Side side = sideDist(rng_) ? Side::Buy : Side::Sell;

    Price_t midPrice = (bbo.bestBid + bbo.bestAsk) / 2;
    if (midPrice == 0) midPrice = toFixedPrice(150.0);

    std::uniform_int_distribution<int> spreadDist(-5, 5);
    Price_t tickOffset = spreadDist(rng_) * (PRICE_SCALE / 100);
    Price_t price = midPrice + tickOffset;
    if (price <= 0) price = PRICE_SCALE;

    std::uniform_int_distribution<int> qtyDist(5, 100);
    Quantity_t qty = qtyDist(rng_);
    ClOrdId_t clOrdId = nextClOrdId_++;

    oePipe_.push<NewOrderEvent>(clOrdId, symIdx, side, OrderType::Limit,
                                 TimeInForce::Day, price, qty, member.sessionId);
}

int AITraderActor::totalOrderCount() const {
    int total = 0;
    for (auto& m : members_) total += m.orderCount;
    return total;
}

} // namespace eunex
