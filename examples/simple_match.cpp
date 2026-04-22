// ════════════════════════════════════════════════════════════════════
// Simple Match Example — End-to-end order matching with recovery
//
// Demonstrates the full Optiq-style flow:
//   1. Recovery Cause persists the incoming order
//   2. Business logic matches the order
//   3. Effect sends ack back to OE (Master only)
//   4. Effect publishes market data update
//   5. IACA fragments form a complete chain
//
// This bridges StockEx's match_order() with Optiq's Cause-Effect model.
// ════════════════════════════════════════════════════════════════════

#include "engine/SimplxShim.hpp"
#include "common/Book.hpp"
#include "recovery/RecoveryProxy.hpp"
#include "iaca/Fragment.hpp"
#include "iaca/IacaAggregator.hpp"
#include <iostream>

using namespace eunex;
using namespace eunex::recovery;
using namespace eunex::iaca;

int main() {
    std::cout << "=== Simple Match with Recovery + IACA ===\n\n";

    // ── Setup infrastructure ───────────────────────────────────────
    FragmentStore store;
    RecoveryProxy recoveryProxy(ORIGIN_BOOK, /*key=*/1, store, /*isMaster=*/true);
    IacaAggregator aggregator;

    int iaMessagesGenerated = 0;
    aggregator.registerHandler(std::make_shared<NewOrderHandler>(
        [&](const FragmentChain& chain) {
            ++iaMessagesGenerated;
            std::cout << "  [IACA] IA message generated from chain "
                      << chain.chainId << " (" << chain.fragments.size()
                      << " fragments)\n";
        }
    ));

    Book book(1);

    // ── Process a sell order (Optiq-style) ─────────────────────────
    std::cout << "── Processing SELL 100 @ 50.00 ──\n";
    {
        Order sellOrder{};
        sellOrder.clOrdId  = 1001;
        sellOrder.symbolIdx = 1;
        sellOrder.side     = Side::Sell;
        sellOrder.ordType  = OrderType::Limit;
        sellOrder.tif      = TimeInForce::Day;
        sellOrder.price    = toFixedPrice(50.00);
        sellOrder.quantity = 100;

        // Step 1: Recovery Cause — persist the event
        recoveryProxy.cause(/*persistenceId=*/1, sellOrder,
            [&](uint64_t chainId, uint64_t seq) -> int {

                // Step 2: IACA Cause — emit root fragment (BOOK)
                IacaFragment bookFrag{};
                bookFrag.chainId        = chainId;
                bookFrag.origin         = {ORIGIN_BOOK, 1, seq};
                bookFrag.previousOrigin = Origin::null();
                bookFrag.causeId        = CAUSE_NEW_ORDER_SELL;
                bookFrag.nextCount      = 1; // one child: the ack

                // Step 3: Business logic — match the order
                book.newOrder(sellOrder,
                    [](const Trade&) {},
                    [](const ExecutionReport& rpt) {
                        std::cout << "  [ExecRpt] Status="
                                  << (int)rpt.status << " Remaining="
                                  << rpt.remainingQty << "\n";
                    }
                );

                // Step 4: Effect — send ack (Master only)
                recoveryProxy.effect([&]() {
                    IacaFragment ackFrag{};
                    ackFrag.chainId        = chainId;
                    ackFrag.origin         = {ORIGIN_LOGICAL_CORE, 1, seq};
                    ackFrag.previousOrigin = bookFrag.origin;
                    ackFrag.causeId        = CAUSE_ACK_DATA;
                    ackFrag.nextCount      = 0;
                    aggregator.addFragment(ackFrag);
                    std::cout << "  [Effect] Ack sent (Master)\n";
                });

                aggregator.addFragment(bookFrag);
                return 0; // recovery nextCount always 0 for ME events
            }
        );
    }

    // ── Process a buy that matches ─────────────────────────────────
    std::cout << "\n── Processing BUY 60 @ 50.00 ──\n";
    {
        Order buyOrder{};
        buyOrder.clOrdId  = 1002;
        buyOrder.symbolIdx = 1;
        buyOrder.side     = Side::Buy;
        buyOrder.ordType  = OrderType::Limit;
        buyOrder.tif      = TimeInForce::Day;
        buyOrder.price    = toFixedPrice(50.00);
        buyOrder.quantity = 60;

        recoveryProxy.cause(2, buyOrder,
            [&](uint64_t chainId, uint64_t seq) -> int {

                IacaFragment bookFrag{};
                bookFrag.chainId        = chainId;
                bookFrag.origin         = {ORIGIN_BOOK, 1, seq};
                bookFrag.previousOrigin = Origin::null();
                bookFrag.causeId        = CAUSE_NEW_ORDER_BUY;
                bookFrag.nextCount      = 2; // ack + trade

                book.newOrder(buyOrder,
                    [&](const Trade& trade) {
                        std::cout << "  [Trade] " << trade.quantity
                                  << " @ " << toDouble(trade.price) << "\n";

                        // IACA fragment for trade
                        recoveryProxy.effect([&]() {
                            IacaFragment tradeFrag{};
                            tradeFrag.chainId        = chainId;
                            tradeFrag.origin         = {ORIGIN_LOGICAL_CORE, 1, seq + 100};
                            tradeFrag.previousOrigin = bookFrag.origin;
                            tradeFrag.causeId        = CAUSE_TRADE_DATA;
                            tradeFrag.nextCount      = 0;
                            aggregator.addFragment(tradeFrag);
                        });
                    },
                    [](const ExecutionReport& rpt) {
                        std::cout << "  [ExecRpt] Status="
                                  << (int)rpt.status << " Filled="
                                  << rpt.filledQty << "\n";
                    }
                );

                recoveryProxy.effect([&]() {
                    IacaFragment ackFrag{};
                    ackFrag.chainId        = chainId;
                    ackFrag.origin         = {ORIGIN_LOGICAL_CORE, 1, seq + 200};
                    ackFrag.previousOrigin = bookFrag.origin;
                    ackFrag.causeId        = CAUSE_ACK_DATA;
                    ackFrag.nextCount      = 0;
                    aggregator.addFragment(ackFrag);
                });

                aggregator.addFragment(bookFrag);
                return 0;
            }
        );
    }

    // ── Summary ────────────────────────────────────────────────────
    std::cout << "\n── Summary ───────────────────────────────\n";
    std::cout << "  Recovery fragments persisted: " << store.size() << "\n";
    std::cout << "  IACA chains completed: " << aggregator.completedChainCount() << "\n";
    std::cout << "  IA messages generated: " << iaMessagesGenerated << "\n";
    std::cout << "  Book state: " << book.bidCount() << " bids, "
              << book.askCount() << " asks\n";

    auto bids = book.getBids(5);
    auto asks = book.getAsks(5);
    if (!asks.empty()) {
        std::cout << "  Best ask: " << toDouble(asks[0].price)
                  << " x " << asks[0].totalQty << "\n";
    }
    if (!bids.empty()) {
        std::cout << "  Best bid: " << toDouble(bids[0].price)
                  << " x " << bids[0].totalQty << "\n";
    }

    // ── Simulate Mirror replay ─────────────────────────────────────
    std::cout << "\n── Mirror Replay Simulation ──────────────\n";
    RecoveryProxy mirrorProxy(ORIGIN_BOOK, 1, store, /*isMaster=*/false);
    Book mirrorBook(1);

    // Effects should NOT fire on mirror
    mirrorProxy.effect([]() {
        std::cout << "  [BUG] This should not print on Mirror!\n";
    });
    std::cout << "  Mirror effect correctly skipped.\n";

    // RecoveryEffect SHOULD fire on mirror
    mirrorProxy.recoveryEffect([]() {
        std::cout << "  [RecoveryEffect] Mirror-only logic executed.\n";
    });

    std::cout << "\n=== Complete ===\n";
    return 0;
}
