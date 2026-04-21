#pragma once
// ════════════════════════════════════════════════════════════════════
// FIXGatewayActor — C++ FIX 4.4 TCP acceptor
//
// Optiq equivalent: OEG FIX Gateway (FIX 4.4 acceptor for OE frontal)
//
// Runs a TCP accept loop on a background thread. Each connected client
// gets a recv thread that parses FIX messages and enqueues them to the
// actor's mailbox. The actor thread pushes NewOrderEvent / CancelEvent
// to OEGateway via Event::Pipe.
//
// Supported messages:
//   Logon (35=A), Logout (35=5), Heartbeat (35=0),
//   NewOrderSingle (35=D), OrderCancelRequest (35=F),
//   OrderCancelReplaceRequest (35=G)
//
// Outbound:
//   ExecutionReport (35=8) via onEvent(ExecReportEvent)
// ════════════════════════════════════════════════════════════════════

#include "engine/SimplxShim.hpp"
#include "actors/Events.hpp"
#include "net/SocketCompat.hpp"
#include <thread>
#include <atomic>
#include <unordered_map>
#include <mutex>
#include <string>
#include <vector>
#include <functional>

namespace eunex {

struct FIXSession {
    socket_t    sock;
    SessionId_t sessionId;
    std::string senderCompId;
    int         msgSeqNum;
    bool        loggedOn;
    std::thread recvThread;
};

class FIXGatewayActor : public tredzone::Actor {
public:
    struct Service : tredzone::AsyncService {};

    FIXGatewayActor(const tredzone::ActorId& oeGatewayId, uint16_t port = 9001);
    ~FIXGatewayActor();

    void onEvent(const ExecReportEvent& event);

    void stop();
    bool isRunning() const { return running_.load(); }
    int clientCount() const;

private:
    tredzone::Actor::Event::Pipe oePipe_;
    uint16_t port_;
    std::atomic<bool> running_{false};
    socket_t listenSock_ = INVALID_SOCK;
    std::thread acceptThread_;
    SocketInit sockInit_;

    mutable std::mutex sessionMutex_;
    std::unordered_map<SessionId_t, std::unique_ptr<FIXSession>> sessions_;
    SessionId_t nextSessionId_ = 100;

    std::unordered_map<SymbolIndex_t, std::string> symbolNames_;

    void acceptLoop();
    void clientRecvLoop(SessionId_t sessionId);

    using TagMap = std::unordered_map<int, std::string>;
    static std::vector<TagMap> parseFIXMessages(const std::string& data);
    static std::string getTag(const TagMap& msg, int tag, const std::string& def = "");

    void handleLogon(FIXSession& sess, const TagMap& msg);
    void handleNewOrderSingle(FIXSession& sess, const TagMap& msg);
    void handleCancelRequest(FIXSession& sess, const TagMap& msg);
    void handleCancelReplaceRequest(FIXSession& sess, const TagMap& msg);

    void sendFIX(FIXSession& sess, const std::string& msgType,
                 const std::vector<std::pair<int, std::string>>& fields);
    void sendExecReport(SessionId_t sessionId, const ExecReportEvent& rpt);

public:
    static SymbolIndex_t symbolFromString(const std::string& sym);
    static std::string symbolToString(SymbolIndex_t idx);
};

} // namespace eunex
