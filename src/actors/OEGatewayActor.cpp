#include "actors/OEGatewayActor.hpp"

namespace eunex {

OEGatewayActor::OEGatewayActor() {
    registerEventHandler<ExecReportEvent>(*this);
}

void OEGatewayActor::mapSymbol(SymbolIndex_t symbolIdx,
                                const tredzone::ActorId& bookActorId) {
    symbolMap_[symbolIdx] = bookActorId;
}

void OEGatewayActor::submitNewOrder(ClOrdId_t clOrdId, SymbolIndex_t symbolIdx,
                                     Side side, OrderType ordType, TimeInForce tif,
                                     Price_t price, Quantity_t qty, SessionId_t session) {
    auto it = symbolMap_.find(symbolIdx);
    if (it == symbolMap_.end()) {
        std::cerr << "OEGateway: unknown symbol " << symbolIdx << "\n";
        return;
    }

    Event::Pipe pipe(*this, it->second);
    pipe.push<NewOrderEvent>(clOrdId, symbolIdx, side, ordType, tif, price, qty, session);
}

void OEGatewayActor::submitCancel(OrderId_t orderId, ClOrdId_t origClOrdId,
                                   SymbolIndex_t symbolIdx, SessionId_t session) {
    auto it = symbolMap_.find(symbolIdx);
    if (it == symbolMap_.end()) return;

    Event::Pipe pipe(*this, it->second);
    pipe.push<CancelOrderEvent>(orderId, origClOrdId, symbolIdx, session);
}

void OEGatewayActor::submitModify(OrderId_t orderId, ClOrdId_t origClOrdId,
                                   SymbolIndex_t symbolIdx, Price_t newPrice,
                                   Quantity_t newQty, SessionId_t session) {
    auto it = symbolMap_.find(symbolIdx);
    if (it == symbolMap_.end()) return;

    Event::Pipe pipe(*this, it->second);
    pipe.push<ModifyOrderEvent>(orderId, origClOrdId, symbolIdx, newPrice, newQty, session);
}

void OEGatewayActor::onEvent(const ExecReportEvent& event) {
    reports_.push_back(event);
}

} // namespace eunex
