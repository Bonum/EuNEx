#pragma once
// ════════════════════════════════════════════════════════════════════
// AITraderActor — Automated trading members
//
// Optiq equivalent: Clearing House member trading obligations
//
// 10 AI members (MBR01-MBR10) with 3 strategies:
//   momentum, mean_reversion, random
//
// Uses Actor::Callback for periodic trading decisions (~30s intervals).
// Receives BookUpdateEvent for BBO data and TradeEvent for price history.
// Pushes NewOrderEvent to OEGActor.
// ════════════════════════════════════════════════════════════════════

#include "engine/SimplxShim.hpp"
#include "actors/Events.hpp"
#include <vector>
#include <random>
#include <string>
#include <unordered_map>

namespace eunex {

enum class Strategy : uint8_t {
    Momentum = 0,
    MeanReversion = 1,
    Random = 2
};

struct AITraderMember {
    MemberId_t  memberId;
    SessionId_t sessionId;
    char        name[8];
    Strategy    strategy;
    int         orderCount;
};

class AITraderActor : public tredzone::Actor, public tredzone::Actor::Callback {
public:
    struct Service : tredzone::AsyncService {};

    AITraderActor(const tredzone::ActorId& oeGatewayId,
                  const std::vector<SymbolIndex_t>& symbols);

    void onEvent(const BookUpdateEvent& event);
    void onEvent(const TradeEvent& event);
    void onEvent(const ExecReportEvent& event);

    void onCallback() override;

    int totalOrderCount() const;

private:
    static constexpr int NUM_MEMBERS = 10;
    static constexpr int TRADE_INTERVAL_MS = 30000;
    static constexpr int MAX_PRICE_HISTORY = 50;

    tredzone::Actor::Event::Pipe oePipe_;
    std::vector<SymbolIndex_t> symbols_;
    std::vector<AITraderMember> members_;
    ClOrdId_t nextClOrdId_ = 50000;

    struct BBO {
        Price_t bestBid = 0;
        Price_t bestAsk = 0;
    };
    std::unordered_map<SymbolIndex_t, BBO> bbos_;
    std::unordered_map<SymbolIndex_t, std::vector<Price_t>> priceHistory_;

    std::mt19937 rng_;

    void initMembers();
    void tradeRound();
    void submitOrder(const AITraderMember& member, SymbolIndex_t symIdx);

    void strategyMomentum(const AITraderMember& member, SymbolIndex_t symIdx,
                          const BBO& bbo, const std::vector<Price_t>& history);
    void strategyMeanReversion(const AITraderMember& member, SymbolIndex_t symIdx,
                               const BBO& bbo, const std::vector<Price_t>& history);
    void strategyRandom(const AITraderMember& member, SymbolIndex_t symIdx,
                        const BBO& bbo);
};

} // namespace eunex
