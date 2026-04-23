#pragma once
#include "engine/SimplxShim.hpp"
#include "common/Book.hpp"
#include "actors/Events.hpp"
#include "persistence/KafkaBus.hpp"
#include <optional>

namespace eunex {

class MECoreActor : public tredzone::Actor {
public:
    struct Service : tredzone::AsyncService {};

    MECoreActor(SymbolIndex_t symbolIdx,
                   const tredzone::ActorId& oeGatewayId,
                   const tredzone::ActorId& marketDataId,
                   const tredzone::ActorId& clearingHouseId = tredzone::ActorId{},
                   KafkaBus* kafkaBus = nullptr);

    void onEvent(const NewOrderEvent& event);
    void onEvent(const CancelOrderEvent& event);
    void onEvent(const ModifyOrderEvent& event);

private:
    Book book_;
    tredzone::Actor::Event::Pipe oePipe_;
    tredzone::Actor::Event::Pipe mdPipe_;
    std::optional<tredzone::Actor::Event::Pipe> chPipe_;
    KafkaBus* kafka_ = nullptr;

    void publishBookUpdate();
};

} // namespace eunex
