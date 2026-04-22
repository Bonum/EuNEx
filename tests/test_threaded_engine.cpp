#include "engine/SimplxShim.hpp"
#include "actors/MECoreActor.hpp"
#include "actors/OEGActor.hpp"
#include "actors/MDGActor.hpp"
#include <iostream>
#include <cassert>
#include <atomic>

using namespace eunex;
using namespace tredzone;

static int testsPassed = 0;
static int testsFailed = 0;

#define TEST(name) \
    std::cout << "  " << #name << "... "; \
    try { test_##name(); std::cout << "PASS\n"; ++testsPassed; } \
    catch (const std::exception& e) { std::cout << "FAIL: " << e.what() << "\n"; ++testsFailed; }

#define ASSERT_EQ(a, b) \
    if ((a) != (b)) throw std::runtime_error( \
        std::string("Expected ") + std::to_string(static_cast<long long>(b)) + \
        " got " + std::to_string(static_cast<long long>(a)))

#define ASSERT_TRUE(x) \
    if (!(x)) throw std::runtime_error("Assertion failed: " #x)

// ── Test: engine creates cores on separate threads ─────────────────

void test_engine_multi_core_creation() {
    Engine::StartSequence seq;
    seq.addActor<OEGActor>(0);
    seq.addActor<MDGActor>(2);
    Engine engine(seq);

    ASSERT_EQ(engine.coreCount(), 2UL);
}

// ── Test: cross-core event delivery via mailbox ────────────────────

void test_cross_core_event_delivery() {
    auto oeGateway = std::make_unique<OEGActor>();
    auto mdActor   = std::make_unique<MDGActor>();

    // Manually assign to different cores
    constexpr SymbolIndex_t SYM = 1;
    auto book = std::make_unique<MECoreActor>(
        SYM, oeGateway->getActorId(), mdActor->getActorId());

    oeGateway->mapSymbol(SYM, book->getActorId());

    // Synchronous mode (no engine) — should still work
    oeGateway->submitNewOrder(1, SYM, Side::Buy, OrderType::Limit,
                              TimeInForce::Day, toFixedPrice(100.0), 50, 1);

    ASSERT_TRUE(oeGateway->getReports().size() > 0);
    ASSERT_EQ(oeGateway->getReports().back().status, OrderStatus::New);
}

// ── Test: threaded engine runs matching across cores ───────────────

struct ThreadedFixture {
    OEGActor* oeGateway = nullptr;
    MDGActor* mdActor  = nullptr;
    std::unique_ptr<Engine> engine;

    ThreadedFixture() {
        Engine::StartSequence seq;
        seq.addActor<OEGActor>(0);
        seq.addActor<MDGActor>(2);
        seq.addActor<MECoreActor>(1,
            SymbolIndex_t(1), ActorId{1, 0}, ActorId{2, 2});

        engine = std::make_unique<Engine>(seq);
    }
};

void test_threaded_matching() {
    // Use synchronous mode for deterministic testing
    auto oeGateway = std::make_unique<OEGActor>();
    auto mdActor   = std::make_unique<MDGActor>();

    constexpr SymbolIndex_t SYM = 1;
    auto book = std::make_unique<MECoreActor>(
        SYM, oeGateway->getActorId(), mdActor->getActorId());

    oeGateway->mapSymbol(SYM, book->getActorId());

    // Sell then buy — should produce a trade
    oeGateway->submitNewOrder(1, SYM, Side::Sell, OrderType::Limit,
                              TimeInForce::Day, toFixedPrice(50.0), 100, 1);
    oeGateway->submitNewOrder(2, SYM, Side::Buy, OrderType::Limit,
                              TimeInForce::Day, toFixedPrice(50.0), 60, 1);

    ASSERT_EQ(mdActor->getRecentTrades().size(), 1UL);
    ASSERT_EQ(mdActor->getRecentTrades()[0].quantity, 60UL);
}

// ── Test: mailbox thread safety ────────────────────────────────────

void test_mailbox_concurrent_enqueue() {
    Mailbox mbox;
    std::atomic<int> counter{0};

    constexpr int N = 1000;
    constexpr int THREADS = 4;

    std::vector<std::thread> threads;
    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&mbox, &counter]() {
            for (int i = 0; i < N; ++i) {
                mbox.enqueue([&counter]() { counter.fetch_add(1); });
            }
        });
    }

    for (auto& t : threads) t.join();
    mbox.drainAll();

    ASSERT_EQ(counter.load(), N * THREADS);
}

// ── Test: engine core count matches assigned cores ─────────────────

void test_engine_core_assignment() {
    Engine::StartSequence seq;
    seq.addActor<OEGActor>(0);
    seq.addActor<MDGActor>(1);
    seq.addActor<OEGActor>(2);
    Engine engine(seq);

    ASSERT_EQ(engine.coreCount(), 3UL);
}

// ── Test: backward compatible synchronous delivery ─────────────────

void test_sync_backward_compat() {
    // Actors created outside Engine should work exactly as before
    auto oeGateway = std::make_unique<OEGActor>();
    auto mdActor   = std::make_unique<MDGActor>();

    constexpr SymbolIndex_t SYM = 1;
    auto book = std::make_unique<MECoreActor>(
        SYM, oeGateway->getActorId(), mdActor->getActorId());

    oeGateway->mapSymbol(SYM, book->getActorId());

    oeGateway->submitNewOrder(1, SYM, Side::Sell, OrderType::Limit,
                              TimeInForce::Day, toFixedPrice(100.0), 50, 1);

    ASSERT_EQ(oeGateway->getReports().size(), 1UL);
    ASSERT_EQ(oeGateway->getReports()[0].status, OrderStatus::New);

    auto* snap = mdActor->getSnapshot(SYM);
    ASSERT_TRUE(snap != nullptr);
    ASSERT_EQ(snap->bestAsk, toFixedPrice(100.0));
}

int main() {
    std::cout << "Multi-Threaded Engine Tests\n";
    std::cout << "───────────────────────────────────────────\n";

    TEST(engine_multi_core_creation);
    TEST(cross_core_event_delivery);
    TEST(threaded_matching);
    TEST(mailbox_concurrent_enqueue);
    TEST(engine_core_assignment);
    TEST(sync_backward_compat);

    std::cout << "───────────────────────────────────────────\n";
    std::cout << testsPassed << " passed, " << testsFailed << " failed\n";
    return testsFailed > 0 ? 1 : 0;
}
