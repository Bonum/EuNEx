#pragma once
// ====================================================================
// KafkaBus -- Multi-topic Kafka publisher for the EuNEx event bus
//
// Optiq equivalent: Kafka Bus (KFK) between ME and downstream
// consumers (MDG, PTB, Clearing, IDS, SATURN).
//
// Topics:
//   eunex.orders           -- incoming orders (NewOrder, Cancel, Modify)
//   eunex.trades           -- executed trades
//   eunex.market-data      -- BBO / depth snapshots
//   eunex.recovery.fragments -- recovery fragments (Master/Mirror)
//
// When EUNEX_USE_KAFKA is OFF, KafkaBus is replaced by a no-op stub
// so actors compile and run without librdkafka.
// ====================================================================

#include "common/Types.hpp"
#include <string>
#include <cstring>
#include <vector>
#include <iostream>
#include <atomic>

#ifdef EUNEX_USE_KAFKA
#include <librdkafka/rdkafkacpp.h>
#endif

namespace eunex {

struct KafkaBusConfig {
    std::string brokers         = "localhost:9092";
    std::string ordersTopic     = "eunex.orders";
    std::string tradesTopic     = "eunex.trades";
    std::string marketDataTopic = "eunex.market-data";
    std::string recoveryTopic   = "eunex.recovery.fragments";
    int         flushTimeoutMs  = 5000;
};

#ifdef EUNEX_USE_KAFKA

class KafkaBus {
public:
    explicit KafkaBus(const KafkaBusConfig& cfg) : config_(cfg) {
        std::string errstr;
        auto conf = std::unique_ptr<RdKafka::Conf>(
            RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL));
        conf->set("bootstrap.servers", config_.brokers, errstr);
        conf->set("queue.buffering.max.messages", "100000", errstr);
        conf->set("linger.ms", "5", errstr);

        producer_.reset(RdKafka::Producer::create(conf.get(), errstr));
        if (!producer_) {
            std::cerr << "[KafkaBus] producer creation failed: " << errstr << "\n";
            return;
        }
        connected_ = true;
        std::cout << "  Kafka Bus:    " << config_.brokers << "\n";
    }

    ~KafkaBus() {
        if (producer_) producer_->flush(config_.flushTimeoutMs);
    }

    void publishTrade(const Trade& trade) {
        std::string key = std::to_string(trade.symbolIdx);
        publish(config_.tradesTopic, key,
                reinterpret_cast<const char*>(&trade), sizeof(Trade));
        tradeCount_.fetch_add(1);
    }

    void publishOrder(const Order& order) {
        std::string key = std::to_string(order.symbolIdx);
        publish(config_.ordersTopic, key,
                reinterpret_cast<const char*>(&order), sizeof(Order));
        orderCount_.fetch_add(1);
    }

    void publishMarketData(SymbolIndex_t sym,
                            Price_t bestBid, Price_t bestAsk,
                            Quantity_t bidQty, Quantity_t askQty) {
        struct MDSnapshot {
            SymbolIndex_t sym;
            Price_t bestBid;
            Price_t bestAsk;
            Quantity_t bidQty;
            Quantity_t askQty;
            Timestamp_ns ts;
        };
        MDSnapshot snap{sym, bestBid, bestAsk, bidQty, askQty, nowNs()};
        std::string key = std::to_string(sym);
        publish(config_.marketDataTopic, key,
                reinterpret_cast<const char*>(&snap), sizeof(snap));
        mdCount_.fetch_add(1);
    }

    void publishRecoveryFragment(const void* data, size_t len,
                                  uint16_t originId, uint32_t originKey) {
        std::string key = std::to_string(originId) + ":" + std::to_string(originKey);
        publish(config_.recoveryTopic, key,
                reinterpret_cast<const char*>(data), len);
    }

    bool isConnected() const { return connected_; }
    size_t tradeCount() const { return tradeCount_.load(); }
    size_t orderCount() const { return orderCount_.load(); }
    size_t mdCount() const { return mdCount_.load(); }

    void flush() {
        if (producer_) producer_->flush(config_.flushTimeoutMs);
    }

private:
    KafkaBusConfig config_;
    std::unique_ptr<RdKafka::Producer> producer_;
    bool connected_ = false;
    std::atomic<size_t> tradeCount_{0};
    std::atomic<size_t> orderCount_{0};
    std::atomic<size_t> mdCount_{0};

    void publish(const std::string& topic, const std::string& key,
                 const char* data, size_t len) {
        if (!producer_) return;
        RdKafka::ErrorCode err = producer_->produce(
            topic, RdKafka::Topic::PARTITION_UA,
            RdKafka::Producer::RK_MSG_COPY,
            const_cast<char*>(data), len,
            key.data(), key.size(),
            0, nullptr);
        if (err != RdKafka::ERR_NO_ERROR) {
            std::cerr << "[KafkaBus] produce to " << topic
                      << " failed: " << RdKafka::err2str(err) << "\n";
        }
        producer_->poll(0);
    }
};

#else // No Kafka -- stub

class KafkaBus {
public:
    explicit KafkaBus(const KafkaBusConfig&) {}
    void publishTrade(const Trade&) {}
    void publishOrder(const Order&) {}
    void publishMarketData(SymbolIndex_t, Price_t, Price_t, Quantity_t, Quantity_t) {}
    void publishRecoveryFragment(const void*, size_t, uint16_t, uint32_t) {}
    bool isConnected() const { return false; }
    size_t tradeCount() const { return 0; }
    size_t orderCount() const { return 0; }
    size_t mdCount() const { return 0; }
    void flush() {}
};

#endif // EUNEX_USE_KAFKA

} // namespace eunex
