#pragma once
// ════════════════════════════════════════════════════════════════════
// IacaAggregator — Fragment chain collector and IA message generator
//
// Optiq equivalent: IacaAggregatorActor.hpp
//   - Receives fragments from all component proxies
//   - Builds CoherentFragmentChain per chainId
//   - Detects chain completion (all nextCount satisfied)
//   - Invokes FragmentHandlers to generate IA SBE messages
// ════════════════════════════════════════════════════════════════════

#include "iaca/Fragment.hpp"
#include <vector>
#include <unordered_map>
#include <functional>
#include <memory>

namespace eunex::iaca {

// ── A chain being assembled ────────────────────────────────────────
struct FragmentChain {
    ChainId_t chainId;
    std::vector<IacaFragment> fragments;
    bool complete = false;

    bool checkComplete() const;
};

// ── Handler interface (like Optiq FragmentHandler) ──────────────────
class FragmentHandler {
public:
    virtual ~FragmentHandler() = default;
    virtual bool canProcess(const FragmentChain& chain) const = 0;
    virtual void process(const FragmentChain& chain) = 0;
};

// ── Aggregator ─────────────────────────────────────────────────────
class IacaAggregator {
public:
    void addFragment(const IacaFragment& fragment);
    void registerHandler(std::shared_ptr<FragmentHandler> handler);

    size_t pendingChainCount() const { return chains_.size(); }
    size_t completedChainCount() const { return completedCount_; }

private:
    std::unordered_map<ChainId_t, FragmentChain> chains_;
    std::vector<std::shared_ptr<FragmentHandler>> handlers_;
    size_t completedCount_ = 0;

    void tryComplete(ChainId_t chainId);
};

// ── Example handler: NewOrder IA message generator ─────────────────
class NewOrderHandler : public FragmentHandler {
public:
    using Callback = std::function<void(const FragmentChain&)>;
    explicit NewOrderHandler(Callback cb) : callback_(std::move(cb)) {}

    bool canProcess(const FragmentChain& chain) const override;
    void process(const FragmentChain& chain) override;

private:
    Callback callback_;
};

} // namespace eunex::iaca
