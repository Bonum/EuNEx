#pragma once
// ════════════════════════════════════════════════════════════════════
// OEGatewayActor — Order Entry Gateway
//
// StockEx equivalent: fix_oeg_server.py + consumer.py
//   - Receives FIX messages, normalizes them, pushes to Kafka
//   - Here: receives external input and routes to OrderBookActor
//
// Optiq equivalent: OEActor
//   - Receives messages from OE frontal (SBE over TCP)
//   - Validates session, routes to LogicalCoreActor
//   - Sends acks/rejects back to OE frontal
// ════════════════════════════════════════════════════════════════════

#include "engine/SimplxShim.hpp"
#include "actors/Events.hpp"
#include <unordered_map>
#include <vector>
#include <iostream>

namespace eunex {

class OEGatewayActor : public tredzone::Actor {
public:
    struct Service : tredzone::AsyncService {};

    OEGatewayActor();

    // Register an OrderBook actor for a symbol
    void mapSymbol(SymbolIndex_t symbolIdx, const tredzone::ActorId& bookActorId);

    // Submit order (called from external interface or test harness)
    void submitNewOrder(ClOrdId_t clOrdId, SymbolIndex_t symbolIdx,
                        Side side, OrderType ordType, TimeInForce tif,
                        Price_t price, Quantity_t qty, SessionId_t session);

    void submitCancel(OrderId_t orderId, ClOrdId_t origClOrdId,
                      SymbolIndex_t symbolIdx, SessionId_t session);

    void submitModify(OrderId_t orderId, ClOrdId_t origClOrdId,
                      SymbolIndex_t symbolIdx, Price_t newPrice,
                      Quantity_t newQty, SessionId_t session);

    void onEvent(const NewOrderEvent& event);
    void onEvent(const CancelOrderEvent& event);
    void onEvent(const ModifyOrderEvent& event);
    void onEvent(const ExecReportEvent& event);

    // Subscribe to execution reports (FIXGateway, AITrader, etc.)
    void addExecReportSubscriber(const tredzone::ActorId& subscriberId);

    // Access received reports (for testing / downstream forwarding)
    const std::vector<ExecReportEvent>& getReports() const { return reports_; }
    void clearReports() { reports_.clear(); }

private:
    std::unordered_map<SymbolIndex_t, tredzone::ActorId> symbolMap_;
    std::vector<tredzone::ActorId> execReportSubscribers_;
    std::vector<ExecReportEvent> reports_;
    ClOrdId_t nextClOrdId_ = 1000;
};

} // namespace eunex
