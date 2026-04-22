#include "actors/FIXAcceptorActor.hpp"
#include <iostream>
#include <sstream>
#include <cstring>
#include <chrono>
#include <iomanip>
#include <ctime>
#include <cstdlib>

namespace eunex {

FIXAcceptorActor::FIXAcceptorActor(const tredzone::ActorId& oeGatewayId, uint16_t port)
    : oePipe_(*this, oeGatewayId)
    , port_(port)
{
    registerEventHandler<ExecReportEvent>(*this);

    symbolNames_[1] = "AAPL";
    symbolNames_[2] = "MSFT";
    symbolNames_[3] = "GOOGL";
    symbolNames_[4] = "EURO50";

    listenSock_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listenSock_ == INVALID_SOCK) {
        std::cerr << "FIXAcceptor: socket() failed\n";
        return;
    }

    setSocketReuseAddr(listenSock_);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);

    if (bind(listenSock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "FIXAcceptor: bind() failed on port " << port_ << "\n";
        closeSocket(listenSock_);
        listenSock_ = INVALID_SOCK;
        return;
    }

    if (listen(listenSock_, 5) != 0) {
        std::cerr << "FIXAcceptor: listen() failed\n";
        closeSocket(listenSock_);
        listenSock_ = INVALID_SOCK;
        return;
    }

    running_ = true;
    acceptThread_ = std::thread(&FIXAcceptorActor::acceptLoop, this);

    std::cout << "FIXAcceptor: listening on port " << port_ << "\n";
}

FIXAcceptorActor::~FIXAcceptorActor() {
    stop();
}

void FIXAcceptorActor::stop() {
    if (!running_.exchange(false)) return;

    if (listenSock_ != INVALID_SOCK) {
        closeSocket(listenSock_);
        listenSock_ = INVALID_SOCK;
    }

    if (acceptThread_.joinable()) acceptThread_.join();

    std::lock_guard<std::mutex> lock(sessionMutex_);
    for (auto& [id, sess] : sessions_) {
        if (sess->sock != INVALID_SOCK) {
#ifdef _WIN32
            shutdown(sess->sock, SD_BOTH);
#else
            shutdown(sess->sock, SHUT_RDWR);
#endif
            closeSocket(sess->sock);
            sess->sock = INVALID_SOCK;
        }
        if (sess->recvThread.joinable()) sess->recvThread.join();
    }
    sessions_.clear();
}

int FIXAcceptorActor::clientCount() const {
    std::lock_guard<std::mutex> lock(sessionMutex_);
    return static_cast<int>(sessions_.size());
}

// ── Accept loop (background thread) ──────────────────────────────

void FIXAcceptorActor::acceptLoop() {
    while (running_) {
        sockaddr_in clientAddr{};
        socklen_t len = sizeof(clientAddr);
        socket_t clientSock = accept(listenSock_,
                                      reinterpret_cast<sockaddr*>(&clientAddr), &len);

        if (clientSock == INVALID_SOCK) {
            if (running_) std::cerr << "FIXAcceptor: accept() failed\n";
            continue;
        }

        std::lock_guard<std::mutex> lock(sessionMutex_);
        SessionId_t sessId = nextSessionId_++;

        auto sess = std::make_unique<FIXSession>();
        sess->sock = clientSock;
        sess->sessionId = sessId;
        sess->msgSeqNum = 1;
        sess->loggedOn = false;

        FIXSession* raw = sess.get();
        sess->recvThread = std::thread(&FIXAcceptorActor::clientRecvLoop, this, sessId);

        sessions_[sessId] = std::move(sess);
        std::cout << "FIXAcceptor: client connected (session " << sessId << ")\n";
    }
}

// ── Client recv loop (per-client thread) ─────────────────────────

void FIXAcceptorActor::clientRecvLoop(SessionId_t sessionId) {
    socket_t sock;
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        auto it = sessions_.find(sessionId);
        if (it == sessions_.end()) return;
        sock = it->second->sock;
    }

    char buf[4096];
    std::string buffer;

    while (running_) {
        int n = recv(sock, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;

        buf[n] = '\0';
        buffer.append(buf, n);

        auto messages = parseFIXMessages(buffer);

        for (auto& msg : messages) {
            std::string msgType = getTag(msg, 35);

            std::lock_guard<std::mutex> lock(sessionMutex_);
            auto it = sessions_.find(sessionId);
            if (it == sessions_.end()) return;
            FIXSession& sess = *it->second;

            if (msgType == "A") {
                handleLogon(sess, msg);
            } else if (msgType == "5") {
                sendFIX(sess, "5", {});
                return;
            } else if (msgType == "0") {
                sendFIX(sess, "0", {});
            } else if (msgType == "D") {
                handleNewOrderSingle(sess, msg);
            } else if (msgType == "F") {
                handleCancelRequest(sess, msg);
            } else if (msgType == "G") {
                handleCancelReplaceRequest(sess, msg);
            }
        }
    }

    std::lock_guard<std::mutex> lock(sessionMutex_);
    auto it = sessions_.find(sessionId);
    if (it != sessions_.end()) {
        closeSocket(it->second->sock);
        it->second->sock = INVALID_SOCK;
    }
}

// ── FIX message parsing ──────────────────────────────────────────

std::vector<FIXAcceptorActor::TagMap> FIXAcceptorActor::parseFIXMessages(const std::string& data) {
    std::vector<TagMap> result;
    std::string remaining = data;

    while (true) {
        auto checkPos = remaining.find("10=");
        if (checkPos == std::string::npos) break;

        auto sohAfterCheck = remaining.find('\x01', checkPos);
        if (sohAfterCheck == std::string::npos) break;

        std::string rawMsg = remaining.substr(0, sohAfterCheck + 1);
        remaining = remaining.substr(sohAfterCheck + 1);

        TagMap tags;
        size_t pos = 0;
        while (pos < rawMsg.size()) {
            auto eqPos = rawMsg.find('=', pos);
            if (eqPos == std::string::npos) break;
            auto sohPos = rawMsg.find('\x01', eqPos);
            if (sohPos == std::string::npos) sohPos = rawMsg.size();

            int tag = std::atoi(rawMsg.substr(pos, eqPos - pos).c_str());
            std::string val = rawMsg.substr(eqPos + 1, sohPos - eqPos - 1);
            tags[tag] = val;
            pos = sohPos + 1;
        }

        if (!tags.empty()) result.push_back(std::move(tags));
    }

    // Put unparsed data back (caller should update buffer)
    const_cast<std::string&>(data) = remaining;
    return result;
}

std::string FIXAcceptorActor::getTag(const TagMap& msg, int tag, const std::string& def) {
    auto it = msg.find(tag);
    return (it != msg.end()) ? it->second : def;
}

// ── FIX message handlers ─────────────────────────────────────────

void FIXAcceptorActor::handleLogon(FIXSession& sess, const TagMap& msg) {
    sess.senderCompId = getTag(msg, 49, "UNKNOWN");
    sess.loggedOn = true;
    std::cout << "FIXAcceptor: Logon from " << sess.senderCompId
              << " (session " << sess.sessionId << ")\n";

    sendFIX(sess, "A", {
        {98, "0"},
        {108, "30"}
    });
}

void FIXAcceptorActor::handleNewOrderSingle(FIXSession& sess, const TagMap& msg) {
    std::string clOrdIdStr = getTag(msg, 11);
    std::string symbol = getTag(msg, 55);
    std::string sideStr = getTag(msg, 54);
    std::string ordTypeStr = getTag(msg, 40);
    std::string priceStr = getTag(msg, 44, "0");
    std::string qtyStr = getTag(msg, 38);
    std::string tifStr = getTag(msg, 59, "0");

    ClOrdId_t clOrdId = std::strtoull(clOrdIdStr.c_str(), nullptr, 10);
    SymbolIndex_t symIdx = symbolFromString(symbol);
    Side side = (sideStr == "1") ? Side::Buy : Side::Sell;
    OrderType ordType = (ordTypeStr == "1") ? OrderType::Market : OrderType::Limit;
    Price_t price = toFixedPrice(std::strtod(priceStr.c_str(), nullptr));
    Quantity_t qty = std::strtoull(qtyStr.c_str(), nullptr, 10);

    TimeInForce tif = TimeInForce::Day;
    if (tifStr == "1") tif = TimeInForce::GTC;
    else if (tifStr == "3") tif = TimeInForce::IOC;
    else if (tifStr == "4") tif = TimeInForce::FOK;

    oePipe_.push<NewOrderEvent>(clOrdId, symIdx, side, ordType, tif, price, qty, sess.sessionId);
}

void FIXAcceptorActor::handleCancelRequest(FIXSession& sess, const TagMap& msg) {
    std::string origClOrdIdStr = getTag(msg, 41);
    std::string orderIdStr = getTag(msg, 37, "0");
    std::string symbol = getTag(msg, 55);

    ClOrdId_t origClOrdId = std::strtoull(origClOrdIdStr.c_str(), nullptr, 10);
    OrderId_t orderId = std::strtoull(orderIdStr.c_str(), nullptr, 10);
    SymbolIndex_t symIdx = symbolFromString(symbol);

    oePipe_.push<CancelOrderEvent>(orderId, origClOrdId, symIdx, sess.sessionId);
}

void FIXAcceptorActor::handleCancelReplaceRequest(FIXSession& sess, const TagMap& msg) {
    std::string orderIdStr = getTag(msg, 37, "0");
    std::string origClOrdIdStr = getTag(msg, 41);
    std::string symbol = getTag(msg, 55);
    std::string priceStr = getTag(msg, 44, "0");
    std::string qtyStr = getTag(msg, 38);

    OrderId_t orderId = std::strtoull(orderIdStr.c_str(), nullptr, 10);
    ClOrdId_t origClOrdId = std::strtoull(origClOrdIdStr.c_str(), nullptr, 10);
    SymbolIndex_t symIdx = symbolFromString(symbol);
    Price_t newPrice = toFixedPrice(std::strtod(priceStr.c_str(), nullptr));
    Quantity_t newQty = std::strtoull(qtyStr.c_str(), nullptr, 10);

    oePipe_.push<ModifyOrderEvent>(orderId, origClOrdId, symIdx, newPrice, newQty, sess.sessionId);
}

// ── Send FIX message ─────────────────────────────────────────────

void FIXAcceptorActor::sendFIX(FIXSession& sess, const std::string& msgType,
                               const std::vector<std::pair<int, std::string>>& fields) {
    std::ostringstream body;
    body << "35=" << msgType << '\x01';
    body << "49=EUNEX" << '\x01';
    body << "56=" << sess.senderCompId << '\x01';
    body << "34=" << sess.msgSeqNum++ << '\x01';

    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    struct tm tmBuf;
#ifdef _WIN32
    gmtime_s(&tmBuf, &t);
#else
    gmtime_r(&t, &tmBuf);
#endif
    char timeBuf[32];
    std::strftime(timeBuf, sizeof(timeBuf), "%Y%m%d-%H:%M:%S", &tmBuf);
    body << "52=" << timeBuf << '\x01';

    for (auto& [tag, val] : fields) {
        body << tag << "=" << val << '\x01';
    }

    std::string bodyStr = body.str();

    std::ostringstream msg;
    msg << "8=FIX.4.4" << '\x01';
    msg << "9=" << bodyStr.size() << '\x01';
    msg << bodyStr;

    std::string raw = msg.str();
    int checksum = 0;
    for (char c : raw) checksum += static_cast<unsigned char>(c);
    checksum %= 256;

    char csStr[8];
    std::snprintf(csStr, sizeof(csStr), "%03d", checksum);
    raw += "10=";
    raw += csStr;
    raw += '\x01';

    send(sess.sock, raw.c_str(), static_cast<int>(raw.size()), 0);
}

// ── Exec report handling ─────────────────────────────────────────

void FIXAcceptorActor::onEvent(const ExecReportEvent& event) {
    sendExecReport(event.sessionId, event);
}

void FIXAcceptorActor::sendExecReport(SessionId_t sessionId, const ExecReportEvent& rpt) {
    std::lock_guard<std::mutex> lock(sessionMutex_);
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end()) return;
    FIXSession& sess = *it->second;
    if (!sess.loggedOn) return;

    auto statusToOrdStatus = [](OrderStatus s) -> std::string {
        switch (s) {
            case OrderStatus::New:             return "0";
            case OrderStatus::PartiallyFilled: return "1";
            case OrderStatus::Filled:          return "2";
            case OrderStatus::Cancelled:       return "4";
            case OrderStatus::Rejected:        return "8";
            default: return "0";
        }
    };

    auto statusToExecType = [](OrderStatus s) -> std::string {
        switch (s) {
            case OrderStatus::New:             return "0";
            case OrderStatus::PartiallyFilled: return "F";
            case OrderStatus::Filled:          return "F";
            case OrderStatus::Cancelled:       return "4";
            case OrderStatus::Rejected:        return "8";
            default: return "0";
        }
    };

    std::vector<std::pair<int, std::string>> fields = {
        {37, std::to_string(rpt.orderId)},
        {11, std::to_string(rpt.clOrdId)},
        {17, std::to_string(rpt.tradeId)},
        {150, statusToExecType(rpt.status)},
        {39, statusToOrdStatus(rpt.status)},
        {14, std::to_string(rpt.filledQty)},
        {151, std::to_string(rpt.remainingQty)},
    };

    if (rpt.lastQty > 0) {
        fields.push_back({31, std::to_string(toDouble(rpt.lastPrice))});
        fields.push_back({32, std::to_string(rpt.lastQty)});
    }

    sendFIX(sess, "8", fields);
}

// ── Symbol mapping ───────────────────────────────────────────────

SymbolIndex_t FIXAcceptorActor::symbolFromString(const std::string& sym) {
    if (sym == "AAPL")   return 1;
    if (sym == "MSFT")   return 2;
    if (sym == "GOOGL")  return 3;
    if (sym == "EURO50") return 4;
    return static_cast<SymbolIndex_t>(std::strtoul(sym.c_str(), nullptr, 10));
}

std::string FIXAcceptorActor::symbolToString(SymbolIndex_t idx) {
    switch (idx) {
        case 1: return "AAPL";
        case 2: return "MSFT";
        case 3: return "GOOGL";
        case 4: return "EURO50";
        default: return std::to_string(idx);
    }
}

} // namespace eunex
