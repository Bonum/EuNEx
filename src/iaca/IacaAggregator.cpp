#include "iaca/IacaAggregator.hpp"
#include <algorithm>

namespace eunex::iaca {

bool FragmentChain::checkComplete() const {
    for (auto& frag : fragments) {
        int childCount = 0;
        for (auto& other : fragments) {
            if (other.previousOrigin == frag.origin)
                ++childCount;
        }
        if (childCount != frag.nextCount)
            return false;
    }
    return !fragments.empty();
}

void IacaAggregator::addFragment(const IacaFragment& fragment) {
    auto& chain = chains_[fragment.chainId];
    chain.chainId = fragment.chainId;
    chain.fragments.push_back(fragment);
    tryComplete(fragment.chainId);
}

void IacaAggregator::registerHandler(std::shared_ptr<FragmentHandler> handler) {
    handlers_.push_back(std::move(handler));
}

void IacaAggregator::tryComplete(ChainId_t chainId) {
    auto it = chains_.find(chainId);
    if (it == chains_.end()) return;

    auto& chain = it->second;
    if (!chain.checkComplete()) return;

    chain.complete = true;
    ++completedCount_;

    for (auto& handler : handlers_) {
        if (handler->canProcess(chain)) {
            handler->process(chain);
        }
    }

    chains_.erase(it);
}

bool NewOrderHandler::canProcess(const FragmentChain& chain) const {
    return std::any_of(chain.fragments.begin(), chain.fragments.end(),
        [](const IacaFragment& f) {
            return f.causeId == CAUSE_NEW_ORDER_BUY || f.causeId == CAUSE_NEW_ORDER_SELL;
        });
}

void NewOrderHandler::process(const FragmentChain& chain) {
    if (callback_) callback_(chain);
}

} // namespace eunex::iaca
