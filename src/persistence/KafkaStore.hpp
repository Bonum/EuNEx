#pragma once
#ifdef EUNEX_USE_KAFKA

#include "persistence/PersistenceStore.hpp"
#include <librdkafka/rdkafkacpp.h>
#include <string>
#include <memory>
#include <mutex>
#include <atomic>
#include <iostream>

namespace eunex::persistence {

struct KafkaConfig {
    std::string brokers = "localhost:9092";
    std::string topic   = "eunex.recovery.fragments";
    std::string groupId = "eunex-recovery";
    int         flushTimeoutMs = 5000;
    int         batchSize = 100;
};

// ── Kafka-backed persistence store ─────────────────────────────────
class KafkaStore : public PersistenceStore {
public:
    explicit KafkaStore(const KafkaConfig& cfg) : config_(cfg) {
        initProducer();
        initConsumer();
    }

    ~KafkaStore() override {
        if (producer_) producer_->flush(config_.flushTimeoutMs);
    }

    void append(const Fragment& frag) override {
        if (!producer_ || !topic_) return;

        std::vector<uint8_t> buf = serialize(frag);

        std::string key = std::to_string(frag.originId) + ":" +
                          std::to_string(frag.originKey);

        RdKafka::ErrorCode err = producer_->produce(
            topic_.get(), RdKafka::Topic::PARTITION_UA,
            RdKafka::Producer::RK_MSG_COPY,
            buf.data(), buf.size(),
            key.data(), key.size(),
            0, nullptr);

        if (err != RdKafka::ERR_NO_ERROR) {
            std::cerr << "[KafkaStore] produce failed: "
                      << RdKafka::err2str(err) << "\n";
        }

        producer_->poll(0);
        count_.fetch_add(1);
    }

    std::vector<Fragment> readAll() const override {
        if (!consumer_) return {};

        std::vector<Fragment> result;
        while (true) {
            auto msg = std::unique_ptr<RdKafka::Message>(
                consumer_->consume(1000));
            if (!msg || msg->err() == RdKafka::ERR__TIMED_OUT ||
                msg->err() == RdKafka::ERR__PARTITION_EOF) {
                break;
            }
            if (msg->err() != RdKafka::ERR_NO_ERROR) break;

            Fragment frag = deserialize(
                static_cast<const uint8_t*>(msg->payload()),
                msg->len());
            result.push_back(frag);
        }
        return result;
    }

    size_t size() const override {
        return count_.load();
    }

    void flush() override {
        if (producer_) producer_->flush(config_.flushTimeoutMs);
    }

    bool isConnected() const { return producer_ != nullptr; }

private:
    KafkaConfig config_;
    std::unique_ptr<RdKafka::Producer> producer_;
    std::unique_ptr<RdKafka::Topic> topic_;
    mutable std::unique_ptr<RdKafka::KafkaConsumer> consumer_;
    std::atomic<size_t> count_{0};

    void initProducer() {
        std::string errstr;
        auto conf = std::unique_ptr<RdKafka::Conf>(
            RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL));

        conf->set("bootstrap.servers", config_.brokers, errstr);
        conf->set("queue.buffering.max.messages", "100000", errstr);
        conf->set("batch.size", std::to_string(config_.batchSize), errstr);

        producer_.reset(RdKafka::Producer::create(conf.get(), errstr));
        if (!producer_) {
            std::cerr << "[KafkaStore] producer creation failed: " << errstr << "\n";
            return;
        }

        auto tconf = std::unique_ptr<RdKafka::Conf>(
            RdKafka::Conf::create(RdKafka::Conf::CONF_TOPIC));
        topic_.reset(RdKafka::Topic::create(producer_.get(), config_.topic,
                                             tconf.get(), errstr));
    }

    void initConsumer() {
        std::string errstr;
        auto conf = std::unique_ptr<RdKafka::Conf>(
            RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL));

        conf->set("bootstrap.servers", config_.brokers, errstr);
        conf->set("group.id", config_.groupId, errstr);
        conf->set("auto.offset.reset", "earliest", errstr);
        conf->set("enable.auto.commit", "false", errstr);

        consumer_.reset(RdKafka::KafkaConsumer::create(conf.get(), errstr));
        if (consumer_) {
            consumer_->subscribe({config_.topic});
        }
    }

    static std::vector<uint8_t> serialize(const Fragment& frag) {
        // Header: fixed fields, then payload
        size_t headerSize = sizeof(uint64_t) * 2 + sizeof(uint16_t) +
                            sizeof(uint32_t) + sizeof(uint8_t) +
                            sizeof(int) + sizeof(size_t);
        std::vector<uint8_t> buf(headerSize + frag.payloadSize);
        uint8_t* p = buf.data();

        std::memcpy(p, &frag.sequenceNumber, sizeof(uint64_t)); p += sizeof(uint64_t);
        std::memcpy(p, &frag.chainId, sizeof(uint64_t));        p += sizeof(uint64_t);
        std::memcpy(p, &frag.originId, sizeof(uint16_t));       p += sizeof(uint16_t);
        std::memcpy(p, &frag.originKey, sizeof(uint32_t));      p += sizeof(uint32_t);
        std::memcpy(p, &frag.persistenceId, sizeof(uint8_t));   p += sizeof(uint8_t);
        std::memcpy(p, &frag.nextCount, sizeof(int));           p += sizeof(int);
        std::memcpy(p, &frag.payloadSize, sizeof(size_t));      p += sizeof(size_t);
        std::memcpy(p, frag.payload, frag.payloadSize);

        return buf;
    }

    static Fragment deserialize(const uint8_t* data, size_t len) {
        Fragment frag{};
        const uint8_t* p = data;

        std::memcpy(&frag.sequenceNumber, p, sizeof(uint64_t)); p += sizeof(uint64_t);
        std::memcpy(&frag.chainId, p, sizeof(uint64_t));        p += sizeof(uint64_t);
        std::memcpy(&frag.originId, p, sizeof(uint16_t));       p += sizeof(uint16_t);
        std::memcpy(&frag.originKey, p, sizeof(uint32_t));      p += sizeof(uint32_t);
        std::memcpy(&frag.persistenceId, p, sizeof(uint8_t));   p += sizeof(uint8_t);
        std::memcpy(&frag.nextCount, p, sizeof(int));           p += sizeof(int);
        std::memcpy(&frag.payloadSize, p, sizeof(size_t));      p += sizeof(size_t);

        size_t remaining = len - static_cast<size_t>(p - data);
        size_t copySize = std::min(frag.payloadSize, remaining);
        copySize = std::min(copySize, sizeof(frag.payload));
        std::memcpy(frag.payload, p, copySize);

        return frag;
    }
};

} // namespace eunex::persistence

#endif // EUNEX_USE_KAFKA
