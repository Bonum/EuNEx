#pragma once
// ════════════════════════════════════════════════════════════════════
// RecoveryProxy — Recovery layer with pluggable persistence
//
// Optiq equivalent: RecoveryProxy.hpp
//   - Wraps every incoming event in a Recovery Cause
//   - Persists fragment via PersistenceStore (in-memory or Kafka)
//   - Gates Effects based on Master/Mirror role
// ════════════════════════════════════════════════════════════════════

#include "common/Types.hpp"
#include <vector>
#include <functional>
#include <cstring>
#include <iostream>

namespace eunex::recovery {

// ── Recovery Fragment (matches Optiq fragment structure) ────────────
struct Fragment {
    uint64_t sequenceNumber;
    uint16_t originId;
    uint32_t originKey;
    uint64_t chainId;
    uint8_t  persistenceId;
    int      nextCount;
    uint8_t  payload[4096];
    size_t   payloadSize;
};

// ── Fragment Store (in-memory, backward compatible) ────────────────
class FragmentStore {
public:
    void append(const Fragment& frag) {
        fragments_.push_back(frag);
    }

    const std::vector<Fragment>& fragments() const { return fragments_; }
    size_t size() const { return fragments_.size(); }
    void clear() { fragments_.clear(); }

private:
    std::vector<Fragment> fragments_;
};

// ── RecoveryProxy ──────────────────────────────────────────────────
class RecoveryProxy {
public:
    RecoveryProxy(uint16_t originId, uint32_t originKey, FragmentStore& store, bool isMaster)
        : originId_(originId), originKey_(originKey), store_(&store), isMaster_(isMaster) {}

    template<typename Payload, typename CauseOp>
    uint64_t cause(uint8_t persistenceId, const Payload& payload, CauseOp&& op) {
        Fragment frag{};
        frag.sequenceNumber = ++sequenceNumber_;
        frag.originId       = originId_;
        frag.originKey      = originKey_;
        frag.chainId        = frag.sequenceNumber;
        frag.persistenceId  = persistenceId;
        frag.payloadSize    = sizeof(Payload);
        std::memcpy(frag.payload, &payload, sizeof(Payload));

        int nextCount = op(frag.chainId, frag.sequenceNumber);
        frag.nextCount = nextCount;

        store_->append(frag);

        return frag.sequenceNumber;
    }

    template<typename Fn, typename... Args>
    void effect(Fn&& fn, Args&&... args) {
        if (isMaster_) {
            fn(std::forward<Args>(args)...);
        }
    }

    template<typename Fn, typename... Args>
    void recoveryEffect(Fn&& fn, Args&&... args) {
        if (!isMaster_) {
            fn(std::forward<Args>(args)...);
        }
    }

    bool isMaster() const { return isMaster_; }
    void setMaster(bool v) { isMaster_ = v; }
    uint64_t lastSequence() const { return sequenceNumber_; }

private:
    uint16_t originId_;
    uint32_t originKey_;
    FragmentStore* store_;
    bool isMaster_;
    uint64_t sequenceNumber_ = 0;
};

} // namespace eunex::recovery
