// ════════════════════════════════════════════════════════════════════
// ClearingHouseActor tests
// ════════════════════════════════════════════════════════════════════

#include "actors/ClearingHouseActor.hpp"
#include "actors/OrderBookActor.hpp"
#include "actors/OEGatewayActor.hpp"
#include "actors/MarketDataActor.hpp"
#include <iostream>
#include <cassert>

using namespace eunex;

static int testsPassed = 0;
static int testsFailed = 0;

#define TEST(name) \
    std::cout << "  " << #name << "... "; \
    try { test_##name(); std::cout << "PASS\n"; ++testsPassed; } \
    catch (const std::exception& e) { std::cout << "FAIL: " << e.what() << "\n"; ++testsFailed; }

#define ASSERT_EQ(a, b) \
    if ((a) != (b)) throw std::runtime_error( \
        std::string("Expected ") + std::to_string(static_cast<long long>(b)) + " got " + std::to_string(static_cast<long long>(a)))

#define ASSERT_TRUE(x) \
    if (!(x)) throw std::runtime_error("Assertion failed: " #x)

// ── Tests ─────────────────────────────────────────────────────────

void test_initial_members() {
    ClearingHouseActor ch;
    auto lb = ch.getLeaderboard();
    ASSERT_EQ(static_cast<int>(lb.size()), 10);
    for (auto& e : lb) {
        ASSERT_EQ(static_cast<int>(e.capital), 100000);
        ASSERT_EQ(static_cast<int>(e.pnl), 0);
        ASSERT_EQ(e.tradeCount, 0);
    }
}

void test_session_mapping() {
    ClearingHouseActor ch;
    ch.mapSession(100, 1);
    ch.mapSession(101, 2);

    Trade t{};
    t.symbolIdx = 1;
    t.price = toFixedPrice(150.0);
    t.quantity = 10;
    t.buySessionId = 100;
    t.sellSessionId = 101;

    TradeEvent evt(t);
    ch.onEvent(evt);

    auto lb = ch.getLeaderboard();
    int totalTrades = 0;
    for (auto& e : lb) totalTrades += e.tradeCount;
    ASSERT_EQ(totalTrades, 2);
}

void test_buy_reduces_capital() {
    ClearingHouseActor ch;
    ch.mapSession(100, 1);

    Trade t{};
    t.symbolIdx = 1;
    t.price = toFixedPrice(100.0);
    t.quantity = 10;
    t.buySessionId = 100;
    t.sellSessionId = 999;

    TradeEvent evt(t);
    ch.onEvent(evt);

    auto* m = ch.getMember(1);
    ASSERT_TRUE(m != nullptr);
    ASSERT_TRUE(m->capital < 100000.0);
    ASSERT_EQ(static_cast<int>(m->capital), 99000);
}

void test_sell_increases_capital() {
    ClearingHouseActor ch;
    ch.mapSession(100, 1);

    Trade t{};
    t.symbolIdx = 1;
    t.price = toFixedPrice(100.0);
    t.quantity = 10;
    t.buySessionId = 999;
    t.sellSessionId = 100;

    TradeEvent evt(t);
    ch.onEvent(evt);

    auto* m = ch.getMember(1);
    ASSERT_TRUE(m != nullptr);
    ASSERT_TRUE(m->capital > 100000.0);
    ASSERT_EQ(static_cast<int>(m->capital), 101000);
}

void test_holdings_tracked() {
    ClearingHouseActor ch;
    ch.mapSession(100, 1);

    Trade t{};
    t.symbolIdx = 1;
    t.price = toFixedPrice(150.0);
    t.quantity = 10;
    t.buySessionId = 100;
    t.sellSessionId = 999;

    TradeEvent evt(t);
    ch.onEvent(evt);

    auto* m = ch.getMember(1);
    ASSERT_TRUE(m != nullptr);
    ASSERT_EQ(static_cast<int>(m->holdings.size()), 1);
    auto hIt = m->holdings.find(1);
    ASSERT_TRUE(hIt != m->holdings.end());
    ASSERT_EQ(hIt->second.quantity, 10);
}

void test_leaderboard_sorted() {
    ClearingHouseActor ch;
    ch.mapSession(100, 1);
    ch.mapSession(101, 2);

    Trade t1{};
    t1.symbolIdx = 1;
    t1.price = toFixedPrice(100.0);
    t1.quantity = 50;
    t1.buySessionId = 100;
    t1.sellSessionId = 101;

    TradeEvent evt1(t1);
    ch.onEvent(evt1);

    auto lb = ch.getLeaderboard();
    ASSERT_TRUE(lb[0].capital >= lb[1].capital);
}

void test_trade_with_clearing_pipe() {
    auto oe = std::make_unique<OEGatewayActor>();
    auto md = std::make_unique<MarketDataActor>();
    auto ch = std::make_unique<ClearingHouseActor>();

    ch->mapSession(1, 1);
    ch->mapSession(2, 2);

    auto book = std::make_unique<OrderBookActor>(
        1, oe->getActorId(), md->getActorId(), ch->getActorId());
    oe->mapSymbol(1, book->getActorId());

    oe->submitNewOrder(1, 1, Side::Sell, OrderType::Limit,
                        TimeInForce::Day, toFixedPrice(100.0), 50, 1);
    oe->submitNewOrder(2, 1, Side::Buy, OrderType::Limit,
                        TimeInForce::Day, toFixedPrice(100.0), 50, 2);

    auto lb = ch->getLeaderboard();
    int totalTrades = 0;
    for (auto& e : lb) totalTrades += e.tradeCount;
    ASSERT_TRUE(totalTrades >= 2);
}

// ── Main ──────────────────────────────────────────────────────────

int main() {
    std::cout << "ClearingHouse Tests\n";
    std::cout << "═══════════════════════════════════════════\n";

    TEST(initial_members);
    TEST(session_mapping);
    TEST(buy_reduces_capital);
    TEST(sell_increases_capital);
    TEST(holdings_tracked);
    TEST(leaderboard_sorted);
    TEST(trade_with_clearing_pipe);

    std::cout << "═══════════════════════════════════════════\n";
    std::cout << testsPassed << " passed, " << testsFailed << " failed\n";
    return testsFailed > 0 ? 1 : 0;
}
