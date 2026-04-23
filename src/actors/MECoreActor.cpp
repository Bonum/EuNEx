#include "actors/MECoreActor.hpp"
#include <iostream>

namespace eunex {

MECoreActor::MECoreActor(SymbolIndex_t symbolIdx,
                               const tredzone::ActorId& oeGatewayId,
                               const tredzone::ActorId& marketDataId,
                               const tredzone::ActorId& clearingHouseId,
                               KafkaBus* kafkaBus)
    : book_(symbolIdx)
    , oePipe_(*this, oeGatewayId)
    , mdPipe_(*this, marketDataId)
    , kafka_(kafkaBus)
{
    if (clearingHouseId.id != 0) {
        chPipe_.emplace(*this, clearingHouseId);
    }
    registerEventHandler<NewOrderEvent>(*this);
    registerEventHandler<CancelOrderEvent>(*this);
    registerEventHandler<ModifyOrderEvent>(*this);
}

void MECoreActor::onEvent(const NewOrderEvent& event) {
    Order order{};
    order.clOrdId    = event.clOrdId;
    order.symbolIdx  = event.symbolIdx;
    order.side       = event.side;
    order.ordType    = event.ordType;
    order.tif        = event.tif;
    order.price      = event.price;
    order.quantity   = event.quantity;
    order.sessionId  = event.sessionId;
    order.stopPrice  = event.stopPrice;

    if (kafka_) kafka_->publishOrder(order);

    auto onTrade = [this](const Trade& trade) {
        mdPipe_.push<TradeEvent>(trade);
        if (chPipe_) chPipe_->push<TradeEvent>(trade);
        if (kafka_) kafka_->publishTrade(trade);
    };
    auto onExec = [this, &event](const ExecutionReport& rpt) {
        oePipe_.push<ExecReportEvent>(rpt, event.sessionId);
    };

    book_.newOrder(order, onTrade, onExec);

    if (book_.stopOrderCount() > 0 && book_.lastTradePrice() != 0) {
        book_.triggerStopOrders(book_.lastTradePrice(), onTrade, onExec);
    }

    publishBookUpdate();
}

void MECoreActor::onEvent(const CancelOrderEvent& event) {
    ExecutionReport rpt{};
    if (book_.cancelOrder(event.orderId, rpt)) {
        oePipe_.push<ExecReportEvent>(rpt, event.sessionId);
        publishBookUpdate();
    } else {
        ExecutionReport reject{event.orderId, event.origClOrdId,
                               OrderStatus::Rejected, 0, 0, 0, 0, 0};
        oePipe_.push<ExecReportEvent>(reject, event.sessionId);
    }
}

void MECoreActor::onEvent(const ModifyOrderEvent& event) {
    ExecutionReport rpt{};
    if (book_.modifyOrder(event.orderId, event.newPrice, event.newQuantity, rpt)) {
        oePipe_.push<ExecReportEvent>(rpt, event.sessionId);
        publishBookUpdate();
    } else {
        ExecutionReport reject{event.orderId, event.origClOrdId,
                               OrderStatus::Rejected, 0, 0, 0, 0, 0};
        oePipe_.push<ExecReportEvent>(reject, event.sessionId);
    }
}

void MECoreActor::publishBookUpdate() {
    BookUpdateEvent update;
    update.symbolIdx = book_.symbolIndex();

    auto bidLevels = book_.getBids(10);
    update.bidDepth = static_cast<int>(bidLevels.size());
    for (int i = 0; i < update.bidDepth; ++i) {
        update.bids[i] = {bidLevels[i].price, bidLevels[i].totalQty};
    }

    auto askLevels = book_.getAsks(10);
    update.askDepth = static_cast<int>(askLevels.size());
    for (int i = 0; i < update.askDepth; ++i) {
        update.asks[i] = {askLevels[i].price, askLevels[i].totalQty};
    }

    mdPipe_.push<BookUpdateEvent>(update);

    if (kafka_) {
        Price_t bb = bidLevels.empty() ? 0 : bidLevels[0].price;
        Price_t ba = askLevels.empty() ? 0 : askLevels[0].price;
        Quantity_t bq = bidLevels.empty() ? 0 : bidLevels[0].totalQty;
        Quantity_t aq = askLevels.empty() ? 0 : askLevels[0].totalQty;
        kafka_->publishMarketData(book_.symbolIndex(), bb, ba, bq, aq);
    }
}

} // namespace eunex
