// ════════════════════════════════════════════════════════════════════
// FIXAcceptorActor tests — protocol parsing and symbol mapping
// ════════════════════════════════════════════════════════════════════

#include "actors/FIXAcceptorActor.hpp"
#include "actors/OEGActor.hpp"
#include "actors/MECoreActor.hpp"
#include "actors/MDGActor.hpp"
#include <iostream>
#include <cassert>
#include <string>

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

void test_symbol_from_string() {
    ASSERT_EQ(FIXAcceptorActor::symbolFromString("AAPL"), 1u);
    ASSERT_EQ(FIXAcceptorActor::symbolFromString("MSFT"), 2u);
    ASSERT_EQ(FIXAcceptorActor::symbolFromString("GOOGL"), 3u);
    ASSERT_EQ(FIXAcceptorActor::symbolFromString("EURO50"), 4u);
}

void test_symbol_to_string() {
    ASSERT_TRUE(FIXAcceptorActor::symbolToString(1) == "AAPL");
    ASSERT_TRUE(FIXAcceptorActor::symbolToString(2) == "MSFT");
    ASSERT_TRUE(FIXAcceptorActor::symbolToString(3) == "GOOGL");
    ASSERT_TRUE(FIXAcceptorActor::symbolToString(4) == "EURO50");
    ASSERT_TRUE(FIXAcceptorActor::symbolToString(99) == "99");
}

void test_fix_gateway_creates() {
    auto oe = std::make_unique<OEGActor>();
    auto fix = std::make_unique<FIXAcceptorActor>(oe->getActorId(), 19010);
    ASSERT_TRUE(fix->isRunning());
    ASSERT_EQ(fix->clientCount(), 0);
    fix->stop();
    ASSERT_TRUE(!fix->isRunning());
}

void test_oe_gateway_routes_new_order_event() {
    auto oe = std::make_unique<OEGActor>();
    auto md = std::make_unique<MDGActor>();
    auto book = std::make_unique<MECoreActor>(1, oe->getActorId(), md->getActorId());
    oe->mapSymbol(1, book->getActorId());

    NewOrderEvent evt(5001, 1, Side::Buy, OrderType::Limit, TimeInForce::Day,
                       toFixedPrice(150.0), 100, 1);
    oe->onEvent(evt);

    ASSERT_TRUE(oe->getReports().size() > 0);
    ASSERT_EQ(oe->getReports().back().status, OrderStatus::New);
}

void test_exec_report_forwarding() {
    auto oe = std::make_unique<OEGActor>();
    auto md = std::make_unique<MDGActor>();
    auto book = std::make_unique<MECoreActor>(1, oe->getActorId(), md->getActorId());
    oe->mapSymbol(1, book->getActorId());

    auto fix = std::make_unique<FIXAcceptorActor>(oe->getActorId(), 19011);
    oe->addExecReportSubscriber(fix->getActorId());

    oe->submitNewOrder(1, 1, Side::Buy, OrderType::Limit,
                        TimeInForce::Day, toFixedPrice(100.0), 50, 1);

    ASSERT_TRUE(oe->getReports().size() > 0);

    fix->stop();
}

// ── Main ──────────────────────────────────────────────────────────

int main() {
    std::cout << "FIXGateway Tests\n";
    std::cout << "═══════════════════════════════════════════\n";

    TEST(symbol_from_string);
    TEST(symbol_to_string);
    TEST(fix_gateway_creates);
    TEST(oe_gateway_routes_new_order_event);
    TEST(exec_report_forwarding);

    std::cout << "═══════════════════════════════════════════\n";
    std::cout << testsPassed << " passed, " << testsFailed << " failed\n";
    return testsFailed > 0 ? 1 : 0;
}
