// ====================================================================
// EuNEx Matching Engine — Main Entry Point
//
// Multi-threaded actor topology (mirrors Optiq architecture):
//
//   Core 0: OEGActor + FIXAcceptorActor
//   Core 1: MECoreActor per symbol (matching engine)
//   Core 2: MDGActor
//   Core 3: ClearingHouseActor + AITraderActor
//
// Optiq equivalent topology:
//   OEActor → LogicalCoreActor (Book) → MDLimit → MDIMP
//                                     → OE Ack (back to OEActor)
//                                     → ClearingHouse (via Kafka/PTB)
// ====================================================================

#include "engine/SimplxShim.hpp"
#include "actors/MECoreActor.hpp"
#include "actors/OEGActor.hpp"
#include "actors/MDGActor.hpp"
#include "actors/ClearingHouseActor.hpp"
#include "actors/FIXAcceptorActor.hpp"
#include "actors/AITraderActor.hpp"
#include "persistence/KafkaBus.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>
#include <cstdlib>

using namespace tredzone;
using namespace eunex;

static std::atomic<bool> g_running{true};

void signalHandler(int) { g_running = false; }

int main() {
    std::cout << "===========================================\n";
    std::cout << "  EuNEx Matching Engine v0.5\n";
    std::cout << "  C++ Actor Architecture (Optiq model)\n";
    std::cout << "===========================================\n\n";

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    // --Kafka Bus (optional) --------------------------------------
    std::unique_ptr<KafkaBus> kafkaBus;
    const char* kafkaBrokers = std::getenv("EUNEX_KAFKA_BROKERS");
    if (kafkaBrokers && kafkaBrokers[0] != '\0') {
        KafkaBusConfig cfg;
        cfg.brokers = kafkaBrokers;
        kafkaBus = std::make_unique<KafkaBus>(cfg);
    }

    // --Symbol definitions ----------------------------------------
    constexpr SymbolIndex_t SYM_AAPL  = 1;
    constexpr SymbolIndex_t SYM_MSFT  = 2;
    constexpr SymbolIndex_t SYM_GOOGL = 3;
    constexpr SymbolIndex_t SYM_EURO50 = 4;

    std::vector<SymbolIndex_t> allSymbols = {SYM_AAPL, SYM_MSFT, SYM_GOOGL, SYM_EURO50};

    // --Core 0: OE Gateway ----------------------------------------
    auto oeGateway = std::make_unique<OEGActor>();

    // --Core 2: Market Data ---------------------------------------
    auto mdActor = std::make_unique<MDGActor>();

    // --Core 3: Clearing House ------------------------------------
    auto chActor = std::make_unique<ClearingHouseActor>();

    // Map AI trader sessions to clearing house members
    for (int i = 0; i < 10; ++i) {
        chActor->mapSession(static_cast<SessionId_t>(200 + i),
                            static_cast<MemberId_t>(i + 1));
    }
    // Map FIX gateway sessions (100-109) to members too
    for (int i = 0; i < 10; ++i) {
        chActor->mapSession(static_cast<SessionId_t>(100 + i),
                            static_cast<MemberId_t>(i + 1));
    }

    // --Core 1: Order Books (per symbol) --------------------------
    KafkaBus* kb = kafkaBus.get();
    auto bookAAPL = std::make_unique<MECoreActor>(
        SYM_AAPL, oeGateway->getActorId(), mdActor->getActorId(), chActor->getActorId(), kb);
    auto bookMSFT = std::make_unique<MECoreActor>(
        SYM_MSFT, oeGateway->getActorId(), mdActor->getActorId(), chActor->getActorId(), kb);
    auto bookGOOGL = std::make_unique<MECoreActor>(
        SYM_GOOGL, oeGateway->getActorId(), mdActor->getActorId(), chActor->getActorId(), kb);
    auto bookEURO50 = std::make_unique<MECoreActor>(
        SYM_EURO50, oeGateway->getActorId(), mdActor->getActorId(), chActor->getActorId(), kb);

    oeGateway->mapSymbol(SYM_AAPL, bookAAPL->getActorId());
    oeGateway->mapSymbol(SYM_MSFT, bookMSFT->getActorId());
    oeGateway->mapSymbol(SYM_GOOGL, bookGOOGL->getActorId());
    oeGateway->mapSymbol(SYM_EURO50, bookEURO50->getActorId());

    // --Core 0: FIX Gateway --------------------------------------
    auto fixGateway = std::make_unique<FIXAcceptorActor>(oeGateway->getActorId(), 9001);

    // --Core 3: AI Trader -----------------------------------------
    auto aiTrader = std::make_unique<AITraderActor>(oeGateway->getActorId(), allSymbols);

    // --Wire exec report subscribers ------------------------------
    oeGateway->addExecReportSubscriber(fixGateway->getActorId());
    oeGateway->addExecReportSubscriber(aiTrader->getActorId());

    // --Print topology --------------------------------------------
    std::cout << "Actor topology:\n";
    std::cout << "  Core 0: OEG (id=" << oeGateway->getActorId().id
              << "), FIXAcceptor (id=" << fixGateway->getActorId().id << ")\n";
    std::cout << "  Core 1: Book AAPL (id=" << bookAAPL->getActorId().id
              << "), MSFT (id=" << bookMSFT->getActorId().id
              << "), GOOGL (id=" << bookGOOGL->getActorId().id
              << "), EURO50 (id=" << bookEURO50->getActorId().id << ")\n";
    std::cout << "  Core 2: MDG (id=" << mdActor->getActorId().id << ")\n";
    std::cout << "  Core 3: CH (id=" << chActor->getActorId().id
              << "), AITrader (id=" << aiTrader->getActorId().id << ")\n\n";

    std::cout << "Services:\n";
    std::cout << "  FIX Gateway:  TCP port 9001\n";
    std::cout << "  AI Traders:   10 members (MBR01-MBR10)\n";
    std::cout << "  Symbols:      AAPL, MSFT, GOOGL, EURO50\n";
    if (kafkaBus && kafkaBus->isConnected()) {
        std::cout << "  Kafka Bus:    " << kafkaBrokers << " (connected)\n";
    } else {
        std::cout << "  Kafka Bus:    disabled (set EUNEX_KAFKA_BROKERS to enable)\n";
    }
    std::cout << "\n";

    // --Seed initial orders for AI to have market data ------------
    std::cout << "Seeding initial order book...\n";
    SessionId_t seedSession = 200;

    struct SeedOrder { SymbolIndex_t sym; Side side; double price; Quantity_t qty; };
    SeedOrder seeds[] = {
        {SYM_AAPL,  Side::Sell, 155.00, 100}, {SYM_AAPL,  Side::Sell, 154.00, 200},
        {SYM_AAPL,  Side::Buy,  153.00, 150}, {SYM_AAPL,  Side::Buy,  152.00, 100},
        {SYM_MSFT,  Side::Sell, 325.00, 100}, {SYM_MSFT,  Side::Sell, 324.00, 150},
        {SYM_MSFT,  Side::Buy,  323.00, 200}, {SYM_MSFT,  Side::Buy,  322.00, 100},
        {SYM_GOOGL, Side::Sell, 142.00, 100}, {SYM_GOOGL, Side::Sell, 141.00, 200},
        {SYM_GOOGL, Side::Buy,  140.00, 150}, {SYM_GOOGL, Side::Buy,  139.00, 100},
        {SYM_EURO50, Side::Sell, 5050.00, 50}, {SYM_EURO50, Side::Sell, 5040.00, 80},
        {SYM_EURO50, Side::Buy,  5030.00, 60}, {SYM_EURO50, Side::Buy,  5020.00, 40},
    };

    ClOrdId_t seedClOrd = 1;
    for (auto& s : seeds) {
        oeGateway->submitNewOrder(seedClOrd++, s.sym, s.side, OrderType::Limit,
                                   TimeInForce::Day, toFixedPrice(s.price), s.qty, seedSession);
    }
    oeGateway->clearReports();

    std::cout << "Initial orders seeded.\n\n";

    // --Run AI trading rounds -------------------------------------
    std::cout << "Running AI trading... (Ctrl+C to stop)\n";
    std::cout << "FIX clients can connect to localhost:9001\n\n";

    int round = 0;
    while (g_running) {
        aiTrader->onCallback();
        round++;

        if (round % 10 == 0) {
            std::cout << "--Round " << round << " --\n";

            auto leaderboard = chActor->getLeaderboard();
            std::cout << "  Leaderboard:\n";
            for (int i = 0; i < std::min(5, static_cast<int>(leaderboard.size())); ++i) {
                auto& e = leaderboard[i];
                std::cout << "    " << e.name
                          << "  Capital=" << static_cast<int>(e.capital)
                          << "  P&L=" << static_cast<int>(e.pnl)
                          << "  Trades=" << e.tradeCount << "\n";
            }

            auto printBBO = [&](SymbolIndex_t sym, const char* name) {
                auto* snap = mdActor->getSnapshot(sym);
                if (snap) {
                    std::cout << "  " << name << ":"
                              << " Bid=" << toDouble(snap->bestBid)
                              << " Ask=" << toDouble(snap->bestAsk)
                              << " Last=" << toDouble(snap->lastTradePrice)
                              << " Trades=" << snap->tradeCount << "\n";
                }
            };

            printBBO(SYM_AAPL, "AAPL");
            printBBO(SYM_MSFT, "MSFT");
            printBBO(SYM_GOOGL, "GOOGL");
            printBBO(SYM_EURO50, "EURO50");

            if (fixGateway->isRunning()) {
                std::cout << "  FIX clients: " << fixGateway->clientCount() << "\n";
            }
            if (kafkaBus && kafkaBus->isConnected()) {
                std::cout << "  Kafka: orders=" << kafkaBus->orderCount()
                          << " trades=" << kafkaBus->tradeCount()
                          << " md=" << kafkaBus->mdCount() << "\n";
            }
            std::cout << "\n";
        }

        std::this_thread::sleep_for(std::chrono::seconds(3));
    }

    // --Shutdown --------------------------------------------------
    std::cout << "\nShutting down...\n";
    fixGateway->stop();

    std::cout << "\n--Final Market Data ---------------------\n";
    for (auto sym : allSymbols) {
        auto* snap = mdActor->getSnapshot(sym);
        if (snap) {
            const char* names[] = {"", "AAPL", "MSFT", "GOOGL", "EURO50"};
            std::cout << "  " << names[sym] << ": "
                      << snap->tradeCount << " trades, last=" << toDouble(snap->lastTradePrice) << "\n";
        }
    }

    std::cout << "\n--Final Leaderboard ---------------------\n";
    auto lb = chActor->getLeaderboard();
    for (auto& e : lb) {
        std::cout << "  " << e.name
                  << "  Capital=" << static_cast<int>(e.capital)
                  << "  P&L=" << static_cast<int>(e.pnl)
                  << "  Trades=" << e.tradeCount
                  << "  Holdings=" << e.holdingCount << "\n";
    }

    std::cout << "\nTrades processed: " << mdActor->getRecentTrades().size() << "\n";

    if (kafkaBus) {
        kafkaBus->flush();
        std::cout << "Kafka totals: orders=" << kafkaBus->orderCount()
                  << " trades=" << kafkaBus->tradeCount()
                  << " md=" << kafkaBus->mdCount() << "\n";
    }

    std::cout << "===========================================\n";
    std::cout << "  Engine stopped.\n";
    return 0;
}
