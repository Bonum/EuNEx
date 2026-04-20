#pragma once
// ════════════════════════════════════════════════════════════════════
// Simplx-Compatible Shim
//
// A lightweight, single-threaded emulation of the Simplx actor
// framework API. Allows development and testing without the real
// Simplx dependency. When EUNEX_REAL_SIMPLX is defined, this file
// is not used — #include "simplx.h" directly.
//
// Emulates: Actor, Event, Event::Pipe, Service, Callback, ActorId,
//           Engine, StartSequence, CoreSet.
//
// Limitations vs real Simplx:
//   - Single-threaded: all actors run on one "core"
//   - No cross-core shared memory, no cache-line optimizations
//   - No real CPU pinning or red/blue zone scheduling
//   - Events delivered synchronously on push (not batched)
// ════════════════════════════════════════════════════════════════════

#ifdef EUNEX_REAL_SIMPLX
#include "simplx.h"
#else

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
#include <unordered_map>
#include <typeindex>
#include <typeinfo>
#include <cassert>
#include <iostream>
#include <thread>
#include <chrono>

namespace tredzone {

// ── Forward declarations ───────────────────────────────────────────
class Actor;
class Engine;

// ── ActorId ────────────────────────────────────────────────────────
struct ActorId {
    uint64_t id = 0;
    uint8_t  coreId = 0;
    bool operator==(const ActorId& o) const { return id == o.id; }
    bool operator!=(const ActorId& o) const { return id != o.id; }
    bool isNull() const { return id == 0; }
};

} // namespace tredzone

namespace std {
template<> struct hash<tredzone::ActorId> {
    size_t operator()(const tredzone::ActorId& a) const { return hash<uint64_t>()(a.id); }
};
}

namespace tredzone {

// ── Global actor registry (shim-only) ──────────────────────────────
class ActorRegistry {
public:
    static ActorRegistry& instance() {
        static ActorRegistry reg;
        return reg;
    }

    uint64_t nextId() { return ++nextId_; }

    void registerActor(const ActorId& id, Actor* actor) {
        actors_[id] = actor;
    }

    void unregisterActor(const ActorId& id) {
        actors_.erase(id);
    }

    Actor* findActor(const ActorId& id) {
        auto it = actors_.find(id);
        return it != actors_.end() ? it->second : nullptr;
    }

    // Service registry
    void registerService(std::type_index tag, const ActorId& id) {
        services_[tag] = id;
    }

    ActorId getService(std::type_index tag) const {
        auto it = services_.find(tag);
        return it != services_.end() ? it->second : ActorId{};
    }

private:
    uint64_t nextId_ = 0;
    std::unordered_map<ActorId, Actor*> actors_;
    std::unordered_map<std::type_index, ActorId> services_;
};

// ── Service tag base ───────────────────────────────────────────────
struct AsyncService {};

// ── Actor base class ───────────────────────────────────────────────
class Actor {
public:
    // ── Event base ─────────────────────────────────────────────────
    struct Event {
        ActorId sourceActorId_;

        const ActorId& getSourceActorId() const { return sourceActorId_; }

        // ── Pipe ───────────────────────────────────────────────────
        class Pipe {
        public:
            Pipe(Actor& source, const ActorId& dest)
                : source_(source), destId_(dest) {}

            template<typename EventT, typename... Args>
            EventT& push(Args&&... args) {
                auto evt = std::make_unique<EventT>(std::forward<Args>(args)...);
                evt->sourceActorId_ = source_.getActorId();
                EventT& ref = *evt;

                Actor* dest = ActorRegistry::instance().findActor(destId_);
                if (dest) {
                    dest->deliverEvent(std::type_index(typeid(EventT)),
                                       static_cast<Event*>(evt.release()));
                } else {
                    source_.deliverUndelivered(std::type_index(typeid(EventT)),
                                               static_cast<Event*>(evt.release()));
                }
                return ref;
            }

            void setDestinationActorId(const ActorId& id) { destId_ = id; }
            const ActorId& getDestinationActorId() const { return destId_; }

        private:
            Actor& source_;
            ActorId destId_;
        };

        // ── Batch (stub) ──────────────────────────────────────────
        class Batch {
        public:
            Batch(Pipe&) {}
            bool isPushCommitted(Pipe&) { return true; }
        };
    };

    // ── Callback ───────────────────────────────────────────────────
    class Callback {
    public:
        virtual ~Callback() = default;
        virtual void onCallback() = 0;
    };

    Actor() {
        actorId_.id = ActorRegistry::instance().nextId();
        actorId_.coreId = 0;
        ActorRegistry::instance().registerActor(actorId_, this);
    }

    virtual ~Actor() {
        ActorRegistry::instance().unregisterActor(actorId_);
    }

    const ActorId& getActorId() const { return actorId_; }
    uint8_t getCore() const { return actorId_.coreId; }

    // Event handler registration
    template<typename EventT, typename HandlerT>
    void registerEventHandler(HandlerT& handler) {
        eventHandlers_[std::type_index(typeid(EventT))] =
            [&handler](Event* rawEvt) {
                handler.onEvent(static_cast<const EventT&>(*rawEvt));
                delete rawEvt;
            };
    }

    template<typename EventT, typename HandlerT>
    void registerUndeliveredEventHandler(HandlerT& handler) {
        undeliveredHandlers_[std::type_index(typeid(EventT))] =
            [&handler](Event* rawEvt) {
                handler.onUndeliveredEvent(static_cast<const EventT&>(*rawEvt));
                delete rawEvt;
            };
    }

    void registerCallback(Callback& cb) {
        pendingCallbacks_.push_back(&cb);
    }

    // Create child actors (same core in shim)
    template<typename ActorT, typename... Args>
    ActorId newUnreferencedActor(Args&&... args) {
        auto actor = std::make_unique<ActorT>(std::forward<Args>(args)...);
        ActorId id = actor->getActorId();
        ownedActors_.push_back(std::move(actor));
        return id;
    }

    template<typename ActorT, typename... Args>
    std::shared_ptr<ActorT> newReferencedActor(Args&&... args) {
        auto actor = std::make_shared<ActorT>(std::forward<Args>(args)...);
        return actor;
    }

    void requestDestroy() { destroyRequested_ = true; }

    // ── ServiceIndex (accessed via getEngine()) ────────────────────
    class ServiceIndex {
    public:
        template<typename ServiceTag>
        const ActorId& getServiceActorId() const {
            static ActorId id = ActorRegistry::instance()
                .getService(std::type_index(typeid(ServiceTag)));
            return id;
        }
    };

    // Shim: engine reference not needed, service index is global
    struct EngineProxy {
        ServiceIndex serviceIndex;
        ServiceIndex& getServiceIndex() { return serviceIndex; }
    };

    EngineProxy& getEngine() {
        static EngineProxy proxy;
        return proxy;
    }

    void processPendingCallbacks() {
        auto cbs = std::move(pendingCallbacks_);
        pendingCallbacks_.clear();
        for (auto* cb : cbs) {
            cb->onCallback();
        }
    }

private:
    friend class Engine;

    ActorId actorId_;
    bool destroyRequested_ = false;

    using EventHandler = std::function<void(Event*)>;
    std::unordered_map<std::type_index, EventHandler> eventHandlers_;
    std::unordered_map<std::type_index, EventHandler> undeliveredHandlers_;
    std::vector<Callback*> pendingCallbacks_;
    std::vector<std::unique_ptr<Actor>> ownedActors_;

    void deliverEvent(std::type_index type, Event* evt) {
        auto it = eventHandlers_.find(type);
        if (it != eventHandlers_.end()) {
            it->second(evt);
        } else {
            delete evt;
        }
    }

    void deliverUndelivered(std::type_index type, Event* evt) {
        auto it = undeliveredHandlers_.find(type);
        if (it != undeliveredHandlers_.end()) {
            it->second(evt);
        } else {
            delete evt;
        }
    }
};

// ── Engine ─────────────────────────────────────────────────────────

class Engine {
public:
    struct CoreSet {
        void set(int) {}
    };

    static CoreSet FullCoreSet() { return CoreSet{}; }

    struct StartSequence {
        StartSequence() = default;
        StartSequence(const CoreSet&) {}

        template<typename ActorT, typename... Args>
        void addActor(int coreId, Args&&... args) {
            factories_.push_back([coreId, ... a = std::forward<Args>(args)]() mutable {
                auto actor = std::make_unique<ActorT>(std::move(a)...);
                actor->actorId_.coreId = static_cast<uint8_t>(coreId);
                return actor;
            });
        }

        template<typename ServiceTag, typename ActorT, typename... Args>
        void addServiceActor(int coreId, Args&&... args) {
            factories_.push_back([coreId, ... a = std::forward<Args>(args)]() mutable {
                auto actor = std::make_unique<ActorT>(std::move(a)...);
                actor->actorId_.coreId = static_cast<uint8_t>(coreId);
                ActorRegistry::instance().registerService(
                    std::type_index(typeid(ServiceTag)), actor->getActorId());
                return actor;
            });
        }

        void setRedZoneCore(int) {}
        void setBlueZoneCore(int) {}

        std::vector<std::function<std::unique_ptr<Actor>()>> factories_;
    };

    explicit Engine(StartSequence& seq) {
        for (auto& factory : seq.factories_) {
            actors_.push_back(factory());
        }
        running_ = true;
    }

    ~Engine() {
        running_ = false;
        // Destroy in reverse order (services last)
        while (!actors_.empty()) actors_.pop_back();
    }

    void runFor(std::chrono::milliseconds duration) {
        auto end = std::chrono::steady_clock::now() + duration;
        while (running_ && std::chrono::steady_clock::now() < end) {
            for (auto& actor : actors_) {
                actor->processPendingCallbacks();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    void stop() { running_ = false; }
    bool isRunning() const { return running_; }

private:
    std::vector<std::unique_ptr<Actor>> actors_;
    bool running_ = false;
};

} // namespace tredzone

#endif // EUNEX_REAL_SIMPLX
