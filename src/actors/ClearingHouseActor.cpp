#include "actors/ClearingHouseActor.hpp"
#include <cstring>
#include <algorithm>

namespace eunex {

ClearingHouseActor::ClearingHouseActor() {
    registerEventHandler<TradeEvent>(*this);
    initMembers();
}

void ClearingHouseActor::initMembers() {
    for (int i = 0; i < NUM_MEMBERS; ++i) {
        MemberId_t id = static_cast<MemberId_t>(i + 1);
        MemberState ms{};
        ms.memberId = id;
        std::snprintf(ms.name, sizeof(ms.name), "MBR%02d", i + 1);
        ms.capital = INITIAL_CAPITAL;
        ms.initialCapital = INITIAL_CAPITAL;
        ms.tradeCount = 0;
        ms.distinctSymbols = 0;
        members_[id] = std::move(ms);
    }
}

void ClearingHouseActor::mapSession(SessionId_t sessionId, MemberId_t memberId) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessionToMember_[sessionId] = memberId;
}

void ClearingHouseActor::onEvent(const TradeEvent& event) {
    std::lock_guard<std::mutex> lock(mutex_);

    const Trade& t = event.trade;

    auto buyIt = sessionToMember_.find(t.buySessionId);
    if (buyIt != sessionToMember_.end()) {
        processTradeSide(buyIt->second, t.symbolIdx, t.price, t.quantity, true);
    }

    auto sellIt = sessionToMember_.find(t.sellSessionId);
    if (sellIt != sessionToMember_.end()) {
        processTradeSide(sellIt->second, t.symbolIdx, t.price, t.quantity, false);
    }
}

void ClearingHouseActor::processTradeSide(MemberId_t memberId, SymbolIndex_t symbolIdx,
                                           Price_t price, Quantity_t qty, bool isBuy) {
    auto it = members_.find(memberId);
    if (it == members_.end()) return;

    MemberState& m = it->second;
    m.tradeCount++;

    double cost = toDouble(price) * static_cast<double>(qty);

    if (isBuy) {
        m.capital -= cost;
    } else {
        m.capital += cost;
    }

    auto hIt = m.holdings.find(symbolIdx);
    if (hIt == m.holdings.end()) {
        MemberHolding h{};
        h.symbolIdx = symbolIdx;
        h.quantity = isBuy ? static_cast<int64_t>(qty) : -static_cast<int64_t>(qty);
        h.avgCost = price;
        m.holdings[symbolIdx] = h;
        m.distinctSymbols = static_cast<int>(m.holdings.size());
    } else {
        MemberHolding& h = hIt->second;
        if (isBuy) {
            double prevCost = toDouble(h.avgCost) * static_cast<double>(std::abs(h.quantity));
            h.quantity += static_cast<int64_t>(qty);
            if (h.quantity != 0) {
                h.avgCost = toFixedPrice((prevCost + cost) / static_cast<double>(std::abs(h.quantity)));
            }
        } else {
            h.quantity -= static_cast<int64_t>(qty);
            if (h.quantity == 0) {
                m.holdings.erase(hIt);
                m.distinctSymbols = static_cast<int>(m.holdings.size());
            }
        }
    }
}

std::vector<LeaderboardEntry> ClearingHouseActor::getLeaderboard() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<LeaderboardEntry> entries;
    entries.reserve(members_.size());

    for (auto& [id, ms] : members_) {
        LeaderboardEntry e{};
        e.memberId = ms.memberId;
        std::memcpy(e.name, ms.name, sizeof(e.name));
        e.capital = ms.capital;
        e.pnl = ms.capital - ms.initialCapital;
        e.tradeCount = ms.tradeCount;
        e.holdingCount = static_cast<int>(ms.holdings.size());
        entries.push_back(e);
    }

    std::sort(entries.begin(), entries.end(),
              [](const LeaderboardEntry& a, const LeaderboardEntry& b) {
                  return a.capital > b.capital;
              });

    return entries;
}

const MemberState* ClearingHouseActor::getMember(MemberId_t id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = members_.find(id);
    return (it != members_.end()) ? &it->second : nullptr;
}

} // namespace eunex
