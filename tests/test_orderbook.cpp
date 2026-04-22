// ════════════════════════════════════════════════════════════════════
// OrderBook unit tests
//
// Ported from StockEx matcher/test_matcher.py, adapted for C++
// price-time priority matching with fixed-point prices.
// ════════════════════════════════════════════════════════════════════

#include "common/Book.hpp"
#include <iostream>
#include <cassert>
#include <vector>
#include <cmath>

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

// ── Helpers ────────────────────────────────────────────────────────

static Order makeOrder(Side side, OrderType ot, TimeInForce tif,
                       double price, uint64_t qty, uint64_t clOrdId = 0) {
    Order o{};
    o.clOrdId   = clOrdId;
    o.symbolIdx = 1;
    o.side      = side;
    o.ordType   = ot;
    o.tif       = tif;
    o.price     = toFixedPrice(price);
    o.quantity  = qty;
    return o;
}

static std::vector<Trade> trades;
static std::vector<ExecutionReport> reports;

static void resetCallbacks() {
    trades.clear();
    reports.clear();
}

static auto onTrade = [](const Trade& t) { trades.push_back(t); };
static auto onExec  = [](const ExecutionReport& r) { reports.push_back(r); };

// ── Tests ──────────────────────────────────────────────────────────

void test_limit_buy_rests_on_empty_book() {
    Book book(1);
    resetCallbacks();

    auto order = makeOrder(Side::Buy, OrderType::Limit, TimeInForce::Day, 100.0, 50);
    book.newOrder(order, onTrade, onExec);

    ASSERT_EQ(trades.size(), 0UL);
    ASSERT_EQ(book.bidCount(), 1UL);
    ASSERT_EQ(book.askCount(), 0UL);
    ASSERT_EQ(reports.back().status, OrderStatus::New);
}

void test_limit_sell_rests_on_empty_book() {
    Book book(1);
    resetCallbacks();

    auto order = makeOrder(Side::Sell, OrderType::Limit, TimeInForce::Day, 100.0, 50);
    book.newOrder(order, onTrade, onExec);

    ASSERT_EQ(trades.size(), 0UL);
    ASSERT_EQ(book.bidCount(), 0UL);
    ASSERT_EQ(book.askCount(), 1UL);
}

void test_exact_match() {
    Book book(1);
    resetCallbacks();

    auto sell = makeOrder(Side::Sell, OrderType::Limit, TimeInForce::Day, 50.0, 100);
    book.newOrder(sell, onTrade, onExec);

    resetCallbacks();
    auto buy = makeOrder(Side::Buy, OrderType::Limit, TimeInForce::Day, 50.0, 100);
    book.newOrder(buy, onTrade, onExec);

    ASSERT_EQ(trades.size(), 1UL);
    ASSERT_EQ(trades[0].quantity, 100UL);
    ASSERT_EQ(book.bidCount(), 0UL);
    ASSERT_EQ(book.askCount(), 0UL);
}

void test_partial_fill() {
    Book book(1);
    resetCallbacks();

    auto sell = makeOrder(Side::Sell, OrderType::Limit, TimeInForce::Day, 50.0, 100);
    book.newOrder(sell, onTrade, onExec);

    resetCallbacks();
    auto buy = makeOrder(Side::Buy, OrderType::Limit, TimeInForce::Day, 50.0, 60);
    book.newOrder(buy, onTrade, onExec);

    ASSERT_EQ(trades.size(), 1UL);
    ASSERT_EQ(trades[0].quantity, 60UL);
    ASSERT_EQ(book.askCount(), 1UL);

    auto asks = book.getAsks(1);
    ASSERT_EQ(asks[0].totalQty, 40UL);
}

void test_price_priority() {
    Book book(1);
    resetCallbacks();

    // Two sells at different prices
    auto sell1 = makeOrder(Side::Sell, OrderType::Limit, TimeInForce::Day, 51.0, 50);
    auto sell2 = makeOrder(Side::Sell, OrderType::Limit, TimeInForce::Day, 50.0, 50);
    book.newOrder(sell1, onTrade, onExec);
    book.newOrder(sell2, onTrade, onExec);

    resetCallbacks();
    // Buy should match cheaper sell first
    auto buy = makeOrder(Side::Buy, OrderType::Limit, TimeInForce::Day, 52.0, 30);
    book.newOrder(buy, onTrade, onExec);

    ASSERT_EQ(trades.size(), 1UL);
    ASSERT_EQ(toDouble(trades[0].price), 50.0);
    ASSERT_EQ(trades[0].quantity, 30UL);
}

void test_time_priority() {
    Book book(1);
    resetCallbacks();

    // Two sells at same price — first should match first (FIFO)
    auto sell1 = makeOrder(Side::Sell, OrderType::Limit, TimeInForce::Day, 50.0, 50, 101);
    auto sell2 = makeOrder(Side::Sell, OrderType::Limit, TimeInForce::Day, 50.0, 50, 102);
    book.newOrder(sell1, onTrade, onExec);
    book.newOrder(sell2, onTrade, onExec);

    resetCallbacks();
    auto buy = makeOrder(Side::Buy, OrderType::Limit, TimeInForce::Day, 50.0, 30);
    book.newOrder(buy, onTrade, onExec);

    ASSERT_EQ(trades.size(), 1UL);
    ASSERT_EQ(trades[0].sellClOrdId, 101UL);
}

void test_market_order_matches_any_price() {
    Book book(1);
    resetCallbacks();

    auto sell = makeOrder(Side::Sell, OrderType::Limit, TimeInForce::Day, 999.99, 50);
    book.newOrder(sell, onTrade, onExec);

    resetCallbacks();
    auto buy = makeOrder(Side::Buy, OrderType::Market, TimeInForce::IOC, 0, 30);
    book.newOrder(buy, onTrade, onExec);

    ASSERT_EQ(trades.size(), 1UL);
    ASSERT_EQ(trades[0].quantity, 30UL);
}

void test_market_order_does_not_rest() {
    Book book(1);
    resetCallbacks();

    auto buy = makeOrder(Side::Buy, OrderType::Market, TimeInForce::IOC, 0, 50);
    book.newOrder(buy, onTrade, onExec);

    ASSERT_EQ(trades.size(), 0UL);
    ASSERT_EQ(book.bidCount(), 0UL);
    ASSERT_TRUE(reports.back().status == OrderStatus::Cancelled);
}

void test_ioc_partial_fill_cancel_rest() {
    Book book(1);
    resetCallbacks();

    auto sell = makeOrder(Side::Sell, OrderType::Limit, TimeInForce::Day, 50.0, 30);
    book.newOrder(sell, onTrade, onExec);

    resetCallbacks();
    auto buy = makeOrder(Side::Buy, OrderType::Limit, TimeInForce::IOC, 50.0, 100);
    book.newOrder(buy, onTrade, onExec);

    ASSERT_EQ(trades.size(), 1UL);
    ASSERT_EQ(trades[0].quantity, 30UL);
    ASSERT_EQ(book.bidCount(), 0UL); // unfilled portion cancelled
}

void test_fok_rejected_when_insufficient() {
    Book book(1);
    resetCallbacks();

    auto sell = makeOrder(Side::Sell, OrderType::Limit, TimeInForce::Day, 50.0, 30);
    book.newOrder(sell, onTrade, onExec);

    resetCallbacks();
    auto buy = makeOrder(Side::Buy, OrderType::Limit, TimeInForce::FOK, 50.0, 100);
    book.newOrder(buy, onTrade, onExec);

    ASSERT_EQ(trades.size(), 0UL);
    ASSERT_TRUE(reports.back().status == OrderStatus::Rejected);
    ASSERT_EQ(book.askCount(), 1UL); // sell still on book
}

void test_fok_fills_when_sufficient() {
    Book book(1);
    resetCallbacks();

    auto sell = makeOrder(Side::Sell, OrderType::Limit, TimeInForce::Day, 50.0, 100);
    book.newOrder(sell, onTrade, onExec);

    resetCallbacks();
    auto buy = makeOrder(Side::Buy, OrderType::Limit, TimeInForce::FOK, 50.0, 100);
    book.newOrder(buy, onTrade, onExec);

    ASSERT_EQ(trades.size(), 1UL);
    ASSERT_EQ(trades[0].quantity, 100UL);
    ASSERT_EQ(book.askCount(), 0UL);
}

void test_cancel_order() {
    Book book(1);
    resetCallbacks();

    auto sell = makeOrder(Side::Sell, OrderType::Limit, TimeInForce::Day, 50.0, 100);
    book.newOrder(sell, onTrade, onExec);

    ExecutionReport rpt{};
    bool ok = book.cancelOrder(sell.orderId, rpt);
    ASSERT_TRUE(ok);
    ASSERT_EQ(rpt.status, OrderStatus::Cancelled);
    ASSERT_EQ(book.askCount(), 0UL);
}

void test_cancel_nonexistent() {
    Book book(1);
    ExecutionReport rpt{};
    bool ok = book.cancelOrder(9999, rpt);
    ASSERT_TRUE(!ok);
}

void test_multi_level_sweep() {
    Book book(1);
    resetCallbacks();

    // Three sell levels
    auto s1 = makeOrder(Side::Sell, OrderType::Limit, TimeInForce::Day, 50.0, 20);
    auto s2 = makeOrder(Side::Sell, OrderType::Limit, TimeInForce::Day, 51.0, 20);
    auto s3 = makeOrder(Side::Sell, OrderType::Limit, TimeInForce::Day, 52.0, 20);
    book.newOrder(s1, onTrade, onExec);
    book.newOrder(s2, onTrade, onExec);
    book.newOrder(s3, onTrade, onExec);

    resetCallbacks();
    // Buy sweeps all three levels
    auto buy = makeOrder(Side::Buy, OrderType::Limit, TimeInForce::Day, 52.0, 50);
    book.newOrder(buy, onTrade, onExec);

    ASSERT_EQ(trades.size(), 3UL);
    ASSERT_EQ(trades[0].quantity, 20UL);
    ASSERT_EQ(trades[1].quantity, 20UL);
    ASSERT_EQ(trades[2].quantity, 10UL);
    ASSERT_EQ(book.askCount(), 1UL); // 10 remaining at 52.0
}

// ── Main ───────────────────────────────────────────────────────────
int main() {
    std::cout << "OrderBook Tests\n";
    std::cout << "───────────────────────────────────────────\n";

    TEST(limit_buy_rests_on_empty_book);
    TEST(limit_sell_rests_on_empty_book);
    TEST(exact_match);
    TEST(partial_fill);
    TEST(price_priority);
    TEST(time_priority);
    TEST(market_order_matches_any_price);
    TEST(market_order_does_not_rest);
    TEST(ioc_partial_fill_cancel_rest);
    TEST(fok_rejected_when_insufficient);
    TEST(fok_fills_when_sufficient);
    TEST(cancel_order);
    TEST(cancel_nonexistent);
    TEST(multi_level_sweep);

    std::cout << "───────────────────────────────────────────\n";
    std::cout << testsPassed << " passed, " << testsFailed << " failed\n";
    return testsFailed > 0 ? 1 : 0;
}
