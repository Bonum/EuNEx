#pragma once
// ════════════════════════════════════════════════════════════════════
// ClearingHouseActor — Trade clearing and member position management
//
// Optiq equivalent: Clearing House downstream from Kafka Bus / PTB
//
// Receives TradeEvent from OrderBookActor(s), attributes trades to
// members via SessionId→MemberId mapping, maintains capital and
// holdings per member. Exposes thread-safe getLeaderboard() for
// the Python bridge to read.
// ════════════════════════════════════════════════════════════════════

#include "engine/SimplxShim.hpp"
#include "actors/Events.hpp"
#include <unordered_map>
#include <vector>
#include <mutex>
#include <string>

namespace eunex {

struct MemberState {
    MemberId_t  memberId;
    char        name[8];
    double      capital;
    double      initialCapital;
    int         tradeCount;
    int         distinctSymbols;
    std::unordered_map<SymbolIndex_t, MemberHolding> holdings;
};

struct LeaderboardEntry {
    MemberId_t  memberId;
    char        name[8];
    double      capital;
    double      pnl;
    int         tradeCount;
    int         holdingCount;
};

class ClearingHouseActor : public tredzone::Actor {
public:
    struct Service : tredzone::AsyncService {};

    ClearingHouseActor();

    void onEvent(const TradeEvent& event);

    void mapSession(SessionId_t sessionId, MemberId_t memberId);

    std::vector<LeaderboardEntry> getLeaderboard() const;

    const MemberState* getMember(MemberId_t id) const;

private:
    static constexpr int NUM_MEMBERS = 10;
    static constexpr double INITIAL_CAPITAL = 100000.0;

    std::unordered_map<SessionId_t, MemberId_t> sessionToMember_;
    std::unordered_map<MemberId_t, MemberState> members_;
    mutable std::mutex mutex_;

    void initMembers();
    void processTradeSide(MemberId_t memberId, SymbolIndex_t symbolIdx,
                          Price_t price, Quantity_t qty, bool isBuy);
};

} // namespace eunex
