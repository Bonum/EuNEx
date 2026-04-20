// ════════════════════════════════════════════════════════════════════
// Integration test: full actor-based matching engine
//
// Tests the OEGateway → OrderBook → MarketData actor pipeline.
// Verifies that the actor topology produces correct trades,
// execution reports, and market data snapshots.
// ════════════════════════════════════════════════════════════════════

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

// ── Test fixtures ──────────────────────────────────────────────────

struct Fixture {
    std::unique_ptr<OEGatewayActor>  oeGateway;
    std::unique_ptr<MarketDataActor> mdActor;
    std::unique_ptr<OrderBookActor>  book;
    static constexpr SymbolIndex_t SYM = 1;
    static constexpr SessionId_t SESS = 1;

    Fixture() {
        oeGateway = std::make_unique<OEGatewayActor>();
        mdActor   = std::make_unique<MarketDataActor>();
        book      = std::make_unique<OrderBookActor>(
            SYM, oeGateway->getActorId(), mdActor->getActorId());
        oeGateway->mapSymbol(SYM, book->getActorId());
    }
};

// ── Tests ──────────────────────────────────────────────────────────

void test_order_routed_to_book() {
    Fixture f;
    f.oeGateway->submitNewOrder(1, f.SYM, Side::Buy, OrderType::Limit,
                                 TimeInForce::Day, toFixedPrice(100.0), 50, f.SESS);

    ASSERT_TRUE(f.oeGateway->getReports().size() > 0);
    ASSERT_EQ(f.oeGateway->getReports().back().status, OrderStatus::New);
}

void test_trade_reaches_market_data() {
    Fixture f;

    f.oeGateway->submitNewOrder(1, f.SYM, Side::Sell, OrderType::Limit,
                                 TimeInForce::Day, toFixedPrice(50.0), 100, f.SESS);
    f.oeGateway->submitNewOrder(2, f.SYM, Side::Buy, OrderType::Limit,
                                 TimeInForce::Day, toFixedPrice(50.0), 60, f.SESS);

    ASSERT_EQ(f.mdActor->getRecentTrades().size(), 1UL);
    ASSERT_EQ(f.mdActor->getRecentTrades()[0].quantity, 60UL);
}

void test_market_data_snapshot_updated() {
    Fixture f;

    f.oeGateway->submitNewOrder(1, f.SYM, Side::Sell, OrderType::Limit,
                                 TimeInForce::Day, toFixedPrice(50.0), 100, f.SESS);
    f.oeGateway->submitNewOrder(2, f.SYM, Side::Buy, OrderType::Limit,
                                 TimeInForce::Day, toFixedPrice(49.0), 100, f.SESS);

    auto* snap = f.mdActor->getSnapshot(f.SYM);
    ASSERT_TRUE(snap != nullptr);
    ASSERT_EQ(snap->bestBid, toFixedPrice(49.0));
    ASSERT_EQ(snap->bestAsk, toFixedPrice(50.0));
}

void test_cancel_via_gateway() {
    Fixture f;

    f.oeGateway->submitNewOrder(1, f.SYM, Side::Sell, OrderType::Limit,
                                 TimeInForce::Day, toFixedPrice(50.0), 100, f.SESS);

    auto orderId = f.oeGateway->getReports().back().orderId;
    f.oeGateway->clearReports();

    f.oeGateway->submitCancel(orderId, 1, f.SYM, f.SESS);

    ASSERT_TRUE(f.oeGateway->getReports().size() > 0);
    ASSERT_EQ(f.oeGateway->getReports().back().status, OrderStatus::Cancelled);
}

void test_multiple_fills_generate_reports() {
    Fixture f;

    f.oeGateway->submitNewOrder(1, f.SYM, Side::Sell, OrderType::Limit,
                                 TimeInForce::Day, toFixedPrice(50.0), 30, f.SESS);
    f.oeGateway->submitNewOrder(2, f.SYM, Side::Sell, OrderType::Limit,
                                 TimeInForce::Day, toFixedPrice(51.0), 30, f.SESS);
    f.oeGateway->clearReports();

    f.oeGateway->submitNewOrder(3, f.SYM, Side::Buy, OrderType::Limit,
                                 TimeInForce::Day, toFixedPrice(51.0), 50, f.SESS);

    // Should have reports for: resting sell fill(s) + incoming buy
    ASSERT_TRUE(f.oeGateway->getReports().size() >= 2);
    ASSERT_EQ(f.mdActor->getRecentTrades().size(), 2UL);
}

void test_unknown_symbol_ignored() {
    Fixture f;
    f.oeGateway->submitNewOrder(1, 999, Side::Buy, OrderType::Limit,
                                 TimeInForce::Day, toFixedPrice(50.0), 50, f.SESS);
    ASSERT_EQ(f.oeGateway->getReports().size(), 0UL);
}

// ── Main ───────────────────────────────────────────────────────────

int main() {
    std::cout << "Matching Engine Integration Tests\n";
    std::cout << "───────────────────────────────────────────\n";

    TEST(order_routed_to_book);
    TEST(trade_reaches_market_data);
    TEST(market_data_snapshot_updated);
    TEST(cancel_via_gateway);
    TEST(multiple_fills_generate_reports);
    TEST(unknown_symbol_ignored);

    std::cout << "───────────────────────────────────────────\n";
    std::cout << testsPassed << " passed, " << testsFailed << " failed\n";
    return testsFailed > 0 ? 1 : 0;
}
