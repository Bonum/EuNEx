#pragma once
// ════════════════════════════════════════════════════════════════════
// MECoreActor — The matching engine core
//
// StockEx equivalent: matcher.py (match_order, handle_cancel, handle_amend)
// Optiq equivalent:   LogicalCoreActor + RecoveryHelperCore + Book
//
// One actor per symbol (or group of symbols). Owns the OrderBook,
// processes incoming orders, and emits trades + execution reports.
//
// In Optiq, this would be a LogicalCoreActor with:
//   - RecoveryProxy::Cause for persisting each incoming event
//   - IACA Cause/Effect chain for producing IA messages
//   - Effects gated by Master/Mirror role
//
// Here we implement the core matching with simplified recovery hooks.
// ════════════════════════════════════════════════════════════════════

#include "engine/SimplxShim.hpp"
#include "common/Book.hpp"
#include "actors/Events.hpp"
#include <optional>

namespace eunex {

class MECoreActor : public tredzone::Actor {
public:
    struct Service : tredzone::AsyncService {};

    MECoreActor(SymbolIndex_t symbolIdx,
                   const tredzone::ActorId& oeGatewayId,
                   const tredzone::ActorId& marketDataId,
                   const tredzone::ActorId& clearingHouseId = tredzone::ActorId{});

    void onEvent(const NewOrderEvent& event);
    void onEvent(const CancelOrderEvent& event);
    void onEvent(const ModifyOrderEvent& event);

private:
    Book book_;
    tredzone::Actor::Event::Pipe oePipe_;
    tredzone::Actor::Event::Pipe mdPipe_;
    std::optional<tredzone::Actor::Event::Pipe> chPipe_;

    void publishBookUpdate();
};

} // namespace eunex
