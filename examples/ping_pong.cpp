// ════════════════════════════════════════════════════════════════════
// Ping-Pong Example — Learning the Simplx Actor Model
//
// Two actors exchange events back and forth, demonstrating:
//   - Actor creation
//   - Event definition and registration
//   - Event::Pipe for sending events
//   - getSourceActorId() for replies
//
// This is the first exercise for understanding the actor framework.
// ════════════════════════════════════════════════════════════════════

#include "engine/SimplxShim.hpp"
#include <iostream>
#include <string>

using namespace tredzone;

// ── Events ─────────────────────────────────────────────────────────
struct PingEvent : Actor::Event {
    int counter;
    explicit PingEvent(int c) : counter(c) {}
};

struct PongEvent : Actor::Event {
    int counter;
    explicit PongEvent(int c) : counter(c) {}
};

// ── PongActor: receives Ping, sends Pong back ─────────────────────
class PongActor : public Actor {
public:
    PongActor() {
        registerEventHandler<PingEvent>(*this);
        std::cout << "[Pong] Created on core " << (int)getCore() << "\n";
    }

    void onEvent(const PingEvent& event) {
        std::cout << "[Pong] Received ping #" << event.counter << "\n";
        Event::Pipe pipe(*this, event.getSourceActorId());
        pipe.push<PongEvent>(event.counter);
    }
};

// ── PingActor: sends Ping, receives Pong ───────────────────────────
class PingActor : public Actor {
public:
    PingActor(const ActorId& pongId, int maxRounds)
        : pongPipe_(*this, pongId), maxRounds_(maxRounds)
    {
        registerEventHandler<PongEvent>(*this);
        std::cout << "[Ping] Created on core " << (int)getCore() << "\n";

        // Send first ping
        std::cout << "[Ping] Sending ping #1\n";
        pongPipe_.push<PingEvent>(1);
    }

    void onEvent(const PongEvent& event) {
        std::cout << "[Ping] Received pong #" << event.counter << "\n";
        if (event.counter < maxRounds_) {
            int next = event.counter + 1;
            std::cout << "[Ping] Sending ping #" << next << "\n";
            pongPipe_.push<PingEvent>(next);
        } else {
            std::cout << "[Ping] Done after " << maxRounds_ << " rounds.\n";
        }
    }

private:
    Event::Pipe pongPipe_;
    int maxRounds_;
};

// ── Main ───────────────────────────────────────────────────────────
int main() {
    std::cout << "=== Ping-Pong Actor Example ===\n\n";

    // Create actors (in shim mode, single-threaded)
    auto pong = std::make_unique<PongActor>();
    auto ping = std::make_unique<PingActor>(pong->getActorId(), 5);

    std::cout << "\n=== Complete ===\n";
    return 0;
}
