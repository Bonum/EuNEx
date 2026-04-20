#pragma once
#include "recovery/RecoveryProxy.hpp"
#include <vector>
#include <mutex>

namespace eunex::persistence {

using recovery::Fragment;

// ── Abstract persistence interface ─────────────────────────────────
class PersistenceStore {
public:
    virtual ~PersistenceStore() = default;
    virtual void append(const Fragment& frag) = 0;
    virtual std::vector<Fragment> readAll() const = 0;
    virtual size_t size() const = 0;
    virtual void flush() {}
};

// ── In-memory store (thread-safe, replaces old FragmentStore) ──────
class InMemoryStore : public PersistenceStore {
public:
    void append(const Fragment& frag) override {
        std::lock_guard<std::mutex> lock(mutex_);
        fragments_.push_back(frag);
    }

    std::vector<Fragment> readAll() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return fragments_;
    }

    size_t size() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return fragments_.size();
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        fragments_.clear();
    }

private:
    mutable std::mutex mutex_;
    std::vector<Fragment> fragments_;
};

} // namespace eunex::persistence
