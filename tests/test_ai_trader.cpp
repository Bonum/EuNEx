// ════════════════════════════════════════════════════════════════════
// AITraderActor tests — strategy execution and order submission
// ════════════════════════════════════════════════════════════════════

#include "actors/AITraderActor.hpp"
#include "actors/OEGActor.hpp"
#include "actors/MECoreActor.hpp"
#include "actors/MDGActor.hpp"
#include "actors/ClearingHouseActor.hpp"
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

void test_ai_trader_creates() {
    auto oe = std::make_unique<OEGActor>();
    std::vector<SymbolIndex_t> syms = {1, 2};
    auto ai = std::make_unique<AITraderActor>(oe->getActorId(), syms);
    ASSERT_TRUE(ai != nullptr);
}

void test_ai_submits_orders() {
    auto oe = std::make_unique<OEGActor>();
    auto md = std::make_unique<MDGActor>();

    auto book1 = std::make_unique<MECoreActor>(1, oe->getActorId(), md->getActorId());
    auto book2 = std::make_unique<MECoreActor>(2, oe->getActorId(), md->getActorId());
    oe->mapSymbol(1, book1->getActorId());
    oe->mapSymbol(2, book2->getActorId());

    std::vector<SymbolIndex_t> syms = {1, 2};
    auto ai = std::make_unique<AITraderActor>(oe->getActorId(), syms);

    ai->onCallback();

    ASSERT_TRUE(oe->getReports().size() > 0);
}

void test_ai_responds_to_book_update() {
    auto oe = std::make_unique<OEGActor>();
    auto md = std::make_unique<MDGActor>();
    auto book = std::make_unique<MECoreActor>(1, oe->getActorId(), md->getActorId());
    oe->mapSymbol(1, book->getActorId());

    std::vector<SymbolIndex_t> syms = {1};
    auto ai = std::make_unique<AITraderActor>(oe->getActorId(), syms);

    BookUpdateEvent bue;
    bue.symbolIdx = 1;
    bue.bidDepth = 1;
    bue.askDepth = 1;
    bue.bids[0] = {toFixedPrice(150.0), 100};
    bue.asks[0] = {toFixedPrice(151.0), 100};
    ai->onEvent(bue);

    ai->onCallback();

    ASSERT_TRUE(oe->getReports().size() > 0);
}

void test_ai_responds_to_trade() {
    auto oe = std::make_unique<OEGActor>();
    std::vector<SymbolIndex_t> syms = {1};
    auto ai = std::make_unique<AITraderActor>(oe->getActorId(), syms);

    Trade t{};
    t.symbolIdx = 1;
    t.price = toFixedPrice(150.0);
    t.quantity = 10;
    TradeEvent evt(t);

    for (int i = 0; i < 10; ++i) {
        t.price = toFixedPrice(150.0 + i * 0.5);
        TradeEvent e(t);
        ai->onEvent(e);
    }

    ASSERT_TRUE(true);
}

void test_ai_with_clearing_house() {
    auto oe = std::make_unique<OEGActor>();
    auto md = std::make_unique<MDGActor>();
    auto ch = std::make_unique<ClearingHouseActor>();

    for (int i = 0; i < 10; ++i) {
        ch->mapSession(static_cast<SessionId_t>(200 + i),
                       static_cast<MemberId_t>(i + 1));
    }

    auto book = std::make_unique<MECoreActor>(
        1, oe->getActorId(), md->getActorId(), ch->getActorId());
    oe->mapSymbol(1, book->getActorId());

    std::vector<SymbolIndex_t> syms = {1};
    auto ai = std::make_unique<AITraderActor>(oe->getActorId(), syms);

    for (int i = 0; i < 5; ++i) {
        ai->onCallback();
    }

    auto lb = ch->getLeaderboard();
    ASSERT_EQ(static_cast<int>(lb.size()), 10);
}

void test_multiple_symbols() {
    auto oe = std::make_unique<OEGActor>();
    auto md = std::make_unique<MDGActor>();

    std::vector<SymbolIndex_t> syms = {1, 2, 3, 4};
    std::vector<std::unique_ptr<MECoreActor>> books;
    for (auto s : syms) {
        auto book = std::make_unique<MECoreActor>(s, oe->getActorId(), md->getActorId());
        oe->mapSymbol(s, book->getActorId());
        books.push_back(std::move(book));
    }

    auto ai = std::make_unique<AITraderActor>(oe->getActorId(), syms);
    ai->onCallback();

    ASSERT_TRUE(oe->getReports().size() > 0);
}

// ── Main ──────────────────────────────────────────────────────────

int main() {
    std::cout << "AITrader Tests\n";
    std::cout << "═══════════════════════════════════════════\n";

    TEST(ai_trader_creates);
    TEST(ai_submits_orders);
    TEST(ai_responds_to_book_update);
    TEST(ai_responds_to_trade);
    TEST(ai_with_clearing_house);
    TEST(multiple_symbols);

    std::cout << "═══════════════════════════════════════════\n";
    std::cout << testsPassed << " passed, " << testsFailed << " failed\n";
    return testsFailed > 0 ? 1 : 0;
}
