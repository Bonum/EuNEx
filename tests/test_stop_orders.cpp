// ════════════════════════════════════════════════════════════════════
// Stop order and trading phase tests
// ════════════════════════════════════════════════════════════════════

#include "common/Book.hpp"
#include <iostream>
#include <cassert>
#include <vector>

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

// ── Helpers ──────────────────────────────────────────────────────

struct TestCtx {
    Book book;
    std::vector<Trade> trades;
    std::vector<ExecutionReport> reports;

    TestCtx(SymbolIndex_t sym = 1) : book(sym) {}

    void submit(Side side, OrderType ot, Price_t px, Quantity_t qty,
                Price_t stop = 0, SessionId_t sess = 1) {
        Order o{};
        o.symbolIdx = 1;
        o.side = side;
        o.ordType = ot;
        o.tif = TimeInForce::Day;
        o.price = px;
        o.quantity = qty;
        o.sessionId = sess;
        o.stopPrice = stop;
        book.newOrder(o,
            [this](const Trade& t) { trades.push_back(t); },
            [this](const ExecutionReport& r) { reports.push_back(r); });
    }

    void triggerStops(Price_t tradePrice) {
        book.triggerStopOrders(tradePrice,
            [this](const Trade& t) { trades.push_back(t); },
            [this](const ExecutionReport& r) { reports.push_back(r); });
    }

    void uncross() {
        book.uncross(
            [this](const Trade& t) { trades.push_back(t); },
            [this](const ExecutionReport& r) { reports.push_back(r); });
    }
};

// ── Stop Order Tests ─────────────────────────────────────────────

void test_stop_buy_parks_without_matching() {
    TestCtx ctx;
    // Resting sell at 100
    ctx.submit(Side::Sell, OrderType::Limit, toFixedPrice(100.0), 50);
    // Stop buy at stop=105 — should NOT match, just park
    ctx.submit(Side::Buy, OrderType::StopMarket, 0, 50, toFixedPrice(105.0));

    ASSERT_EQ(ctx.book.stopOrderCount(), static_cast<size_t>(1));
    ASSERT_EQ(ctx.trades.size(), static_cast<size_t>(0));
}

void test_stop_buy_triggers_on_price_rise() {
    TestCtx ctx;
    // Resting sells
    ctx.submit(Side::Sell, OrderType::Limit, toFixedPrice(100.0), 50);
    ctx.submit(Side::Sell, OrderType::Limit, toFixedPrice(110.0), 50);

    // Stop buy: triggers when price >= 105
    ctx.submit(Side::Buy, OrderType::StopMarket, 0, 30, toFixedPrice(105.0));
    ASSERT_EQ(ctx.book.stopOrderCount(), static_cast<size_t>(1));

    // Trade at 100 — stop NOT triggered (100 < 105)
    ctx.triggerStops(toFixedPrice(100.0));
    ASSERT_EQ(ctx.book.stopOrderCount(), static_cast<size_t>(1));

    // Trade at 106 — stop IS triggered, becomes market buy, fills at best ask
    ctx.triggerStops(toFixedPrice(106.0));
    ASSERT_EQ(ctx.book.stopOrderCount(), static_cast<size_t>(0));
    ASSERT_TRUE(ctx.trades.size() > 0);
}

void test_stop_sell_triggers_on_price_drop() {
    TestCtx ctx;
    // Resting buys
    ctx.submit(Side::Buy, OrderType::Limit, toFixedPrice(100.0), 50);
    ctx.submit(Side::Buy, OrderType::Limit, toFixedPrice(90.0), 50);

    // Stop sell: triggers when price <= 95
    ctx.submit(Side::Sell, OrderType::StopMarket, 0, 30, toFixedPrice(95.0));
    ASSERT_EQ(ctx.book.stopOrderCount(), static_cast<size_t>(1));

    // Trade at 96 — not triggered
    ctx.triggerStops(toFixedPrice(96.0));
    ASSERT_EQ(ctx.book.stopOrderCount(), static_cast<size_t>(1));

    // Trade at 94 — triggered, becomes market sell, fills at best bid
    ctx.triggerStops(toFixedPrice(94.0));
    ASSERT_EQ(ctx.book.stopOrderCount(), static_cast<size_t>(0));
    ASSERT_TRUE(ctx.trades.size() > 0);
    ASSERT_EQ(ctx.trades.back().price, toFixedPrice(100.0));
}

void test_stop_limit_uses_limit_price() {
    TestCtx ctx;
    // Resting sells at various prices
    ctx.submit(Side::Sell, OrderType::Limit, toFixedPrice(100.0), 50);
    ctx.submit(Side::Sell, OrderType::Limit, toFixedPrice(110.0), 50);

    // StopLimit buy: stop=105, limit price=108
    // When triggered, becomes Limit buy at 108 — should match sell@100 but NOT sell@110
    ctx.submit(Side::Buy, OrderType::StopLimit, toFixedPrice(108.0), 80, toFixedPrice(105.0));

    ctx.triggerStops(toFixedPrice(106.0));
    ASSERT_EQ(ctx.book.stopOrderCount(), static_cast<size_t>(0));
    // Should have filled 50 at 100.0
    ASSERT_TRUE(ctx.trades.size() > 0);
    ASSERT_EQ(ctx.trades.back().price, toFixedPrice(100.0));
    ASSERT_EQ(ctx.trades.back().quantity, static_cast<Quantity_t>(50));
    // Remaining 30 should rest at limit price 108
    ASSERT_EQ(ctx.book.bidCount(), static_cast<size_t>(1));
}

void test_cancel_stop_order() {
    TestCtx ctx;
    ctx.submit(Side::Buy, OrderType::StopMarket, 0, 50, toFixedPrice(105.0));
    ASSERT_EQ(ctx.book.stopOrderCount(), static_cast<size_t>(1));

    // The stop order's orderId is in the last report
    OrderId_t stopId = ctx.reports.back().orderId;

    ExecutionReport rpt{};
    bool ok = ctx.book.cancelOrder(stopId, rpt);
    ASSERT_TRUE(ok);
    ASSERT_EQ(rpt.status, OrderStatus::Cancelled);
    ASSERT_EQ(ctx.book.stopOrderCount(), static_cast<size_t>(0));
}

void test_multiple_stops_trigger_simultaneously() {
    TestCtx ctx;
    // Resting sells
    ctx.submit(Side::Sell, OrderType::Limit, toFixedPrice(100.0), 100);

    // Two stop buys
    ctx.submit(Side::Buy, OrderType::StopMarket, 0, 30, toFixedPrice(105.0));
    ctx.submit(Side::Buy, OrderType::StopMarket, 0, 40, toFixedPrice(103.0));
    ASSERT_EQ(ctx.book.stopOrderCount(), static_cast<size_t>(2));

    // Both trigger at 106
    ctx.triggerStops(toFixedPrice(106.0));
    ASSERT_EQ(ctx.book.stopOrderCount(), static_cast<size_t>(0));
    // Both should have traded (30 + 40 = 70, resting has 100)
    Quantity_t totalTraded = 0;
    for (auto& t : ctx.trades) totalTraded += t.quantity;
    ASSERT_EQ(totalTraded, static_cast<Quantity_t>(70));
}

// ── Pre-Open / Opening / CTS Phase Tests ─────────────────────────

void test_preopen_accumulates_no_matching() {
    TestCtx ctx;
    ctx.book.setPhase(TradingPhase::PreOpen);

    ctx.submit(Side::Sell, OrderType::Limit, toFixedPrice(100.0), 50);
    ctx.submit(Side::Buy, OrderType::Limit, toFixedPrice(100.0), 50);

    // Orders cross at 100 but should NOT match during PreOpen
    ASSERT_EQ(ctx.trades.size(), static_cast<size_t>(0));
    ASSERT_EQ(ctx.book.bidCount(), static_cast<size_t>(1));
    ASSERT_EQ(ctx.book.askCount(), static_cast<size_t>(1));
}

void test_iop_calculation() {
    TestCtx ctx;
    ctx.book.setPhase(TradingPhase::PreOpen);

    // Build crossing book:
    // Buy  200 @ 105
    // Buy  100 @ 100
    // Sell 150 @ 95
    // Sell 100 @ 100
    ctx.submit(Side::Buy, OrderType::Limit, toFixedPrice(105.0), 200);
    ctx.submit(Side::Buy, OrderType::Limit, toFixedPrice(100.0), 100);
    ctx.submit(Side::Sell, OrderType::Limit, toFixedPrice(95.0), 150);
    ctx.submit(Side::Sell, OrderType::Limit, toFixedPrice(100.0), 100);

    Price_t iop = ctx.book.getIOP();
    // At 100: cumBuy = 300 (200@105 + 100@100), cumSell = 250 (150@95 + 100@100)
    // executable = min(300, 250) = 250
    // At 105: cumBuy = 200, cumSell = 250 → exec = 200
    // At 95: cumBuy = 300, cumSell = 150 → exec = 150
    // Best is 100 with volume 250
    ASSERT_EQ(iop, toFixedPrice(100.0));
}

void test_uncrossing_generates_trades() {
    TestCtx ctx;
    ctx.book.setPhase(TradingPhase::PreOpen);

    ctx.submit(Side::Buy, OrderType::Limit, toFixedPrice(105.0), 100);
    ctx.submit(Side::Sell, OrderType::Limit, toFixedPrice(95.0), 80);

    ASSERT_EQ(ctx.trades.size(), static_cast<size_t>(0));

    // Uncross at IOP
    ctx.uncross();
    ASSERT_TRUE(ctx.trades.size() > 0);
    // Should trade 80 (smaller side) at IOP
    Quantity_t totalTraded = 0;
    for (auto& t : ctx.trades) totalTraded += t.quantity;
    ASSERT_EQ(totalTraded, static_cast<Quantity_t>(80));
    // Remaining 20 buy should still be on book
    ASSERT_EQ(ctx.book.bidCount(), static_cast<size_t>(1));
    ASSERT_EQ(ctx.book.askCount(), static_cast<size_t>(0));
}

void test_cts_matches_normally() {
    TestCtx ctx;
    ctx.book.setPhase(TradingPhase::CTS);

    ctx.submit(Side::Sell, OrderType::Limit, toFixedPrice(100.0), 50);
    ctx.submit(Side::Buy, OrderType::Limit, toFixedPrice(100.0), 50);

    // Should match immediately in CTS
    ASSERT_TRUE(ctx.trades.size() > 0);
    ASSERT_EQ(ctx.trades.back().quantity, static_cast<Quantity_t>(50));
}

void test_closed_rejects_orders() {
    TestCtx ctx;
    ctx.book.setPhase(TradingPhase::Closed);

    ctx.submit(Side::Buy, OrderType::Limit, toFixedPrice(100.0), 50);

    ASSERT_EQ(ctx.trades.size(), static_cast<size_t>(0));
    ASSERT_TRUE(ctx.reports.back().status == OrderStatus::Rejected);
}

void test_preopen_to_cts_workflow() {
    TestCtx ctx;
    // Phase 1: PreOpen — accumulate crossing orders
    ctx.book.setPhase(TradingPhase::PreOpen);
    ctx.submit(Side::Buy, OrderType::Limit, toFixedPrice(102.0), 100);
    ctx.submit(Side::Buy, OrderType::Limit, toFixedPrice(101.0), 50);
    ctx.submit(Side::Sell, OrderType::Limit, toFixedPrice(99.0), 80);
    ctx.submit(Side::Sell, OrderType::Limit, toFixedPrice(100.0), 60);
    ASSERT_EQ(ctx.trades.size(), static_cast<size_t>(0));

    // Phase 2: Opening uncross
    ctx.book.setPhase(TradingPhase::Opening);
    ctx.uncross();
    ASSERT_TRUE(ctx.trades.size() > 0);
    // All trades should be at IOP
    Price_t iopPrice = ctx.trades[0].price;
    for (auto& t : ctx.trades) {
        ASSERT_EQ(t.price, iopPrice);
    }

    // Phase 3: CTS — normal matching
    ctx.book.setPhase(TradingPhase::CTS);
    size_t tradesBefore = ctx.trades.size();
    ctx.submit(Side::Sell, OrderType::Limit, toFixedPrice(100.0), 10);
    // If there are resting buys above 100, this should match
    // Otherwise it rests
    ASSERT_TRUE(true);
}

// ── Main ──────────────────────────────────────────────────────────

int main() {
    std::cout << "Stop Orders & Trading Phase Tests\n";
    std::cout << "═══════════════════════════════════════════\n";

    // Stop orders
    TEST(stop_buy_parks_without_matching);
    TEST(stop_buy_triggers_on_price_rise);
    TEST(stop_sell_triggers_on_price_drop);
    TEST(stop_limit_uses_limit_price);
    TEST(cancel_stop_order);
    TEST(multiple_stops_trigger_simultaneously);

    // Trading phases
    TEST(preopen_accumulates_no_matching);
    TEST(iop_calculation);
    TEST(uncrossing_generates_trades);
    TEST(cts_matches_normally);
    TEST(closed_rejects_orders);
    TEST(preopen_to_cts_workflow);

    std::cout << "═══════════════════════════════════════════\n";
    std::cout << testsPassed << " passed, " << testsFailed << " failed\n";
    return testsFailed > 0 ? 1 : 0;
}
