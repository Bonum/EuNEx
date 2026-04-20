// ════════════════════════════════════════════════════════════════════
// EuNEx Matching Engine — Main Entry Point
//
// Multi-threaded actor topology (mirrors Optiq architecture):
//
//   Core 0: OEGatewayActor (Order Entry — receives external orders)
//   Core 1: OrderBookActor per symbol (matching engine)
//   Core 2: MarketDataActor (publishes book updates, trades)
//
// Optiq equivalent topology:
//   OEActor → LogicalCoreActor (Book) → MDLimit → MDIMP
//                                     → OE Ack (back to OEActor)
// ════════════════════════════════════════════════════════════════════

#include "engine/SimplxShim.hpp"
#include "actors/OrderBookActor.hpp"
#include "actors/OEGatewayActor.hpp"
#include "actors/MarketDataActor.hpp"
#include <iostream>

using namespace tredzone;
using namespace eunex;

int main() {
    std::cout << "═══════════════════════════════════════════\n";
    std::cout << "  EuNEx Matching Engine v0.2\n";
    std::cout << "  Multi-threaded actor engine\n";
    std::cout << "═══════════════════════════════════════════\n\n";

    // ── Build actor topology ───────────────────────────────────────
    auto oeGateway = std::make_unique<OEGatewayActor>();
    auto mdActor   = std::make_unique<MarketDataActor>();

    constexpr SymbolIndex_t SYM_AAPL = 1;
    constexpr SymbolIndex_t SYM_MSFT = 2;

    auto bookAAPL = std::make_unique<OrderBookActor>(
        SYM_AAPL, oeGateway->getActorId(), mdActor->getActorId());
    auto bookMSFT = std::make_unique<OrderBookActor>(
        SYM_MSFT, oeGateway->getActorId(), mdActor->getActorId());

    oeGateway->mapSymbol(SYM_AAPL, bookAAPL->getActorId());
    oeGateway->mapSymbol(SYM_MSFT, bookMSFT->getActorId());

    std::cout << "Actors created:\n";
    std::cout << "  OEGateway  (id=" << oeGateway->getActorId().id << ")\n";
    std::cout << "  MarketData (id=" << mdActor->getActorId().id << ")\n";
    std::cout << "  Book AAPL  (id=" << bookAAPL->getActorId().id << ")\n";
    std::cout << "  Book MSFT  (id=" << bookMSFT->getActorId().id << ")\n\n";

    // ── Submit orders ──────────────────────────────────────────────
    SessionId_t session = 1;
    std::cout << "── Submitting orders ──────────────────────\n\n";

    oeGateway->submitNewOrder(1001, SYM_AAPL, Side::Sell, OrderType::Limit,
                               TimeInForce::Day, toFixedPrice(150.00), 100, session);
    std::cout << "SELL AAPL 100 @ 150.00\n";

    oeGateway->submitNewOrder(1002, SYM_AAPL, Side::Sell, OrderType::Limit,
                               TimeInForce::Day, toFixedPrice(151.00), 50, session);
    std::cout << "SELL AAPL  50 @ 151.00\n";

    oeGateway->submitNewOrder(1003, SYM_AAPL, Side::Buy, OrderType::Limit,
                               TimeInForce::Day, toFixedPrice(150.00), 75, session);
    std::cout << "BUY  AAPL  75 @ 150.00 (should match 75 of sell@150)\n";

    oeGateway->submitNewOrder(1004, SYM_AAPL, Side::Buy, OrderType::Market,
                               TimeInForce::IOC, NULL_PRICE, 30, session);
    std::cout << "BUY  AAPL  30 MARKET IOC (should match 25@150 + 5@151)\n";

    oeGateway->submitNewOrder(1005, SYM_AAPL, Side::Buy, OrderType::Limit,
                               TimeInForce::FOK, toFixedPrice(151.00), 100, session);
    std::cout << "BUY  AAPL 100 @ 151.00 FOK (should be rejected)\n";

    oeGateway->submitNewOrder(2001, SYM_MSFT, Side::Buy, OrderType::Limit,
                               TimeInForce::Day, toFixedPrice(320.50), 200, session);
    std::cout << "BUY  MSFT 200 @ 320.50\n";

    oeGateway->submitNewOrder(2002, SYM_MSFT, Side::Sell, OrderType::Limit,
                               TimeInForce::Day, toFixedPrice(320.50), 150, session);
    std::cout << "SELL MSFT 150 @ 320.50 (should match 150)\n";

    // ── Print results ──────────────────────────────────────────────
    std::cout << "\n── Execution Reports ─────────────────────\n\n";

    auto statusStr = [](OrderStatus s) -> const char* {
        switch (s) {
            case OrderStatus::New: return "NEW";
            case OrderStatus::PartiallyFilled: return "PARTIAL";
            case OrderStatus::Filled: return "FILLED";
            case OrderStatus::Cancelled: return "CANCELLED";
            case OrderStatus::Rejected: return "REJECTED";
            default: return "UNKNOWN";
        }
    };

    for (auto& rpt : oeGateway->getReports()) {
        std::cout << "  ClOrdId=" << rpt.clOrdId
                  << " OrderId=" << rpt.orderId
                  << " Status=" << statusStr(rpt.status)
                  << " Filled=" << rpt.filledQty
                  << " Remaining=" << rpt.remainingQty;
        if (rpt.lastQty > 0) {
            std::cout << " LastPx=" << toDouble(rpt.lastPrice)
                      << " LastQty=" << rpt.lastQty;
        }
        std::cout << "\n";
    }

    // ── Print market data ──────────────────────────────────────────
    std::cout << "\n── Market Data ───────────────────────────\n\n";

    auto printSnapshot = [&](SymbolIndex_t sym, const char* name) {
        auto* snap = mdActor->getSnapshot(sym);
        if (snap) {
            std::cout << "  " << name << ":"
                      << " LastPx=" << toDouble(snap->lastTradePrice)
                      << " BestBid=" << toDouble(snap->bestBid)
                      << " BestAsk=" << toDouble(snap->bestAsk)
                      << " Trades=" << snap->tradeCount << "\n";
        }
    };

    printSnapshot(SYM_AAPL, "AAPL");
    printSnapshot(SYM_MSFT, "MSFT");

    std::cout << "\n  Recent trades: " << mdActor->getRecentTrades().size() << "\n";
    for (auto& t : mdActor->getRecentTrades()) {
        const char* sym = (t.symbolIdx == SYM_AAPL) ? "AAPL" : "MSFT";
        std::cout << "    " << sym << " " << t.quantity << " @ " << toDouble(t.price)
                  << " (buy=" << t.buyOrderId << " sell=" << t.sellOrderId << ")\n";
    }

    std::cout << "\n═══════════════════════════════════════════\n";
    std::cout << "  Engine stopped.\n";
    return 0;
}
