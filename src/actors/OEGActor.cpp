#include "actors/OEGActor.hpp"

namespace eunex {

OEGActor::OEGActor() {
    registerEventHandler<NewOrderEvent>(*this);
    registerEventHandler<CancelOrderEvent>(*this);
    registerEventHandler<ModifyOrderEvent>(*this);
    registerEventHandler<ExecReportEvent>(*this);
}

void OEGActor::mapSymbol(SymbolIndex_t symbolIdx,
                                const tredzone::ActorId& bookActorId) {
    symbolMap_[symbolIdx] = bookActorId;
}

void OEGActor::submitNewOrder(ClOrdId_t clOrdId, SymbolIndex_t symbolIdx,
                                     Side side, OrderType ordType, TimeInForce tif,
                                     Price_t price, Quantity_t qty, SessionId_t session) {
    auto it = symbolMap_.find(symbolIdx);
    if (it == symbolMap_.end()) {
        std::cerr << "OEG: unknown symbol " << symbolIdx << "\n";
        return;
    }

    Event::Pipe pipe(*this, it->second);
    pipe.push<NewOrderEvent>(clOrdId, symbolIdx, side, ordType, tif, price, qty, session);
}

void OEGActor::submitCancel(OrderId_t orderId, ClOrdId_t origClOrdId,
                                   SymbolIndex_t symbolIdx, SessionId_t session) {
    auto it = symbolMap_.find(symbolIdx);
    if (it == symbolMap_.end()) return;

    Event::Pipe pipe(*this, it->second);
    pipe.push<CancelOrderEvent>(orderId, origClOrdId, symbolIdx, session);
}

void OEGActor::submitModify(OrderId_t orderId, ClOrdId_t origClOrdId,
                                   SymbolIndex_t symbolIdx, Price_t newPrice,
                                   Quantity_t newQty, SessionId_t session) {
    auto it = symbolMap_.find(symbolIdx);
    if (it == symbolMap_.end()) return;

    Event::Pipe pipe(*this, it->second);
    pipe.push<ModifyOrderEvent>(orderId, origClOrdId, symbolIdx, newPrice, newQty, session);
}

void OEGActor::onEvent(const NewOrderEvent& event) {
    submitNewOrder(event.clOrdId, event.symbolIdx, event.side, event.ordType,
                    event.tif, event.price, event.quantity, event.sessionId);
}

void OEGActor::onEvent(const CancelOrderEvent& event) {
    submitCancel(event.orderId, event.origClOrdId, event.symbolIdx, event.sessionId);
}

void OEGActor::onEvent(const ModifyOrderEvent& event) {
    submitModify(event.orderId, event.origClOrdId, event.symbolIdx,
                  event.newPrice, event.newQuantity, event.sessionId);
}

void OEGActor::addExecReportSubscriber(const tredzone::ActorId& subscriberId) {
    execReportSubscribers_.push_back(subscriberId);
}

void OEGActor::onEvent(const ExecReportEvent& event) {
    reports_.push_back(event);

    for (auto& subId : execReportSubscribers_) {
        Event::Pipe pipe(*this, subId);
        pipe.push<ExecReportEvent>(event);
    }
}

} // namespace eunex
