#include "actors/OrderBookActor.hpp"
#include <iostream>

namespace eunex {

OrderBookActor::OrderBookActor(SymbolIndex_t symbolIdx,
                               const tredzone::ActorId& oeGatewayId,
                               const tredzone::ActorId& marketDataId,
                               const tredzone::ActorId& clearingHouseId)
    : book_(symbolIdx)
    , oePipe_(*this, oeGatewayId)
    , mdPipe_(*this, marketDataId)
{
    if (clearingHouseId.id != 0) {
        chPipe_.emplace(*this, clearingHouseId);
    }
    registerEventHandler<NewOrderEvent>(*this);
    registerEventHandler<CancelOrderEvent>(*this);
    registerEventHandler<ModifyOrderEvent>(*this);
}

// ── NewOrder ───────────────────────────────────────────────────────
// StockEx: match_order(order, producer)
// Optiq:   RecoveryCause.onInput → CauseOperator → forwardToBook()

void OrderBookActor::onEvent(const NewOrderEvent& event) {
    Order order{};
    order.clOrdId    = event.clOrdId;
    order.symbolIdx  = event.symbolIdx;
    order.side       = event.side;
    order.ordType    = event.ordType;
    order.tif        = event.tif;
    order.price      = event.price;
    order.quantity   = event.quantity;
    order.sessionId  = event.sessionId;

    // In Optiq this would be wrapped in:
    //   RecoveryProxy::Cause → persist to Kafka
    //   IACA Cause → emit BOOK fragment to IacaAggregator
    //   Effect → send ack to OE
    //   Effect → publish limit update to MDLimit

    book_.newOrder(order,
        [this](const Trade& trade) {
            mdPipe_.push<TradeEvent>(trade);
            if (chPipe_) chPipe_->push<TradeEvent>(trade);
        },
        // Execution report — send back to OE Gateway
        [this, &event](const ExecutionReport& rpt) {
            oePipe_.push<ExecReportEvent>(rpt, event.sessionId);
        }
    );

    publishBookUpdate();
}

// ── Cancel ─────────────────────────────────────────────────────────
// StockEx: handle_cancel(msg, producer)

void OrderBookActor::onEvent(const CancelOrderEvent& event) {
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

// ── Modify (Cancel-Replace) ────────────────────────────────────────
// StockEx: handle_amend(msg, producer)

void OrderBookActor::onEvent(const ModifyOrderEvent& event) {
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

// ── Publish book snapshot to MarketData actor ──────────────────────
// StockEx: the Dashboard reads orderbook via REST GET /orderbook/<symbol>
// Optiq:   publishLimitEffect → push to MDLimitLogicalCoreHandler

void OrderBookActor::publishBookUpdate() {
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
}

} // namespace eunex
