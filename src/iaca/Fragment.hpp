#pragma once
// ════════════════════════════════════════════════════════════════════
// IACA Fragment definitions
//
// Optiq equivalent: Fragment structure from IacaDirectory.hpp
//   - Each processing step emits a fragment
//   - Fragments form a tree via previousOrigin / nextCount
//   - Complete trees are assembled into IA SBE messages
// ════════════════════════════════════════════════════════════════════

#include "common/Types.hpp"
#include <cstdint>

namespace eunex::iaca {

struct Origin {
    uint16_t originId;
    uint32_t key;
    uint64_t sequenceNumber;

    bool isNull() const { return originId == 0xFFFF && key == 0 && sequenceNumber == 0; }

    static Origin null() { return {0xFFFF, 0, 0}; }

    bool operator==(const Origin& o) const {
        return originId == o.originId && key == o.key && sequenceNumber == o.sequenceNumber;
    }
};

using ChainId_t = uint64_t;

// Origin IDs matching Optiq component names
enum OriginComponent : uint16_t {
    ORIGIN_BOOK          = 1,
    ORIGIN_LOGICAL_CORE  = 2,
    ORIGIN_MD_LIMIT      = 3,
    ORIGIN_MD_IMP        = 4,
    ORIGIN_OE_ACTOR      = 5
};

// Cause IDs identifying the payload type
enum CauseId : int16_t {
    CAUSE_NEW_ORDER_BUY      = 1,
    CAUSE_NEW_ORDER_SELL     = 2,
    CAUSE_CANCEL_ORDER       = 3,
    CAUSE_MODIFY_ORDER       = 4,
    CAUSE_ACK_DATA           = 10,
    CAUSE_TRADE_DATA         = 11,
    CAUSE_LIMIT_UPDATE       = 20,
    CAUSE_IMP_RESULT         = 21,
    CAUSE_MARKET_STATUS      = 30
};

struct IacaFragment {
    ChainId_t chainId;
    Origin    origin;
    Origin    previousOrigin;
    int       nextCount;
    CauseId   causeId;
    uint8_t   payload[2048];
    size_t    payloadSize;
};

// Identity data embedded in cross-actor events (Optiq: FragmentIdentityData)
struct FragmentIdentityData {
    ChainId_t iacaChainId;
    Origin    iacaOrigin;
    ChainId_t recoveryChainId;
    Origin    recoveryOrigin;
};

} // namespace eunex::iaca
