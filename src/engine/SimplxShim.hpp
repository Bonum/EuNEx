#pragma once
// ════════════════════════════════════════════════════════════════════
// Simplx-Compatible Actor Engine
//
// Multi-threaded actor framework emulating the Simplx API.
// Each logical core runs on its own OS thread with a mailbox queue
// for cross-core event delivery.
//
// Modes:
//   - Synchronous: events delivered inline (for unit tests)
//   - Threaded: events queued to destination core's mailbox
//
// The Engine controls the mode: actors created outside an Engine
// use synchronous delivery; Engine::runFor() uses threaded mode.
//
// Emulates: Actor, Event, Event::Pipe, Service, Callback, ActorId,
//           Engine, StartSequence, CoreSet.
// ════════════════════════════════════════════════════════════════════

#ifdef EUNEX_REAL_SIMPLX
#include "simplx.h"
#else

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
#include <deque>
#include <unordered_map>
#include <map>
#include <typeindex>
#include <typeinfo>
#include <cassert>
#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>

namespace tredzone {

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

// ── Thread-safe mailbox for cross-core event delivery ──────────────
class Mailbox {
public:
    using Task = std::function<void()>;

    void enqueue(Task task) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(std::move(task));
        }
        cv_.notify_one();
    }

    void drainAll() {
        std::deque<Task> batch;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            batch.swap(queue_);
        }
        for (auto& task : batch) task();
    }

    bool waitAndDrain(std::chrono::milliseconds timeout) {
        std::deque<Task> batch;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (queue_.empty()) {
                cv_.wait_for(lock, timeout, [this] { return !queue_.empty() || shutdown_; });
            }
            if (shutdown_ && queue_.empty()) return false;
            batch.swap(queue_);
        }
        for (auto& task : batch) task();
        return true;
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            shutdown_ = true;
        }
        cv_.notify_all();
    }

    size_t pending() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Task> queue_;
    bool shutdown_ = false;
};

// ── Global actor registry ──────────────────────────────────────────
class ActorRegistry {
public:
    static ActorRegistry& instance() {
        static ActorRegistry reg;
        return reg;
    }

    uint64_t nextId() { return ++nextId_; }

    void registerActor(const ActorId& id, Actor* actor) {
        std::lock_guard<std::mutex> lock(mutex_);
        actors_[id] = actor;
    }

    void unregisterActor(const ActorId& id) {
        std::lock_guard<std::mutex> lock(mutex_);
        actors_.erase(id);
    }

    Actor* findActor(const ActorId& id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = actors_.find(id);
        return it != actors_.end() ? it->second : nullptr;
    }

    void registerService(std::type_index tag, const ActorId& id) {
        std::lock_guard<std::mutex> lock(mutex_);
        services_[tag] = id;
    }

    ActorId getService(std::type_index tag) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = services_.find(tag);
        return it != services_.end() ? it->second : ActorId{};
    }

    // Core mailbox management
    Mailbox* getMailbox(uint8_t coreId) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = mailboxes_.find(coreId);
        return it != mailboxes_.end() ? it->second : nullptr;
    }

    void registerMailbox(uint8_t coreId, Mailbox* mbox) {
        std::lock_guard<std::mutex> lock(mutex_);
        mailboxes_[coreId] = mbox;
    }

    void unregisterMailbox(uint8_t coreId) {
        std::lock_guard<std::mutex> lock(mutex_);
        mailboxes_.erase(coreId);
    }

    bool isThreadedMode() const { return threadedMode_.load(); }
    void setThreadedMode(bool v) { threadedMode_.store(v); }

private:
    std::atomic<uint64_t> nextId_{0};
    mutable std::mutex mutex_;
    std::unordered_map<ActorId, Actor*> actors_;
    std::unordered_map<std::type_index, ActorId> services_;
    std::unordered_map<uint8_t, Mailbox*> mailboxes_;
    std::atomic<bool> threadedMode_{false};
};

// ── Service tag base ───────────────────────────────────────────────
struct AsyncService {};

// ── Actor base class ───────────────────────────────────────────────
class Actor {
public:
    struct Event {
        ActorId sourceActorId_;
        const ActorId& getSourceActorId() const { return sourceActorId_; }

        class Pipe {
        public:
            Pipe(Actor& source, const ActorId& dest)
                : source_(source), destId_(dest) {}

            template<typename EventT, typename... Args>
            EventT& push(Args&&... args) {
                auto evt = std::make_unique<EventT>(std::forward<Args>(args)...);
                evt->sourceActorId_ = source_.getActorId();
                EventT& ref = *evt;

                auto& reg = ActorRegistry::instance();
                Actor* dest = reg.findActor(destId_);

                if (!dest) {
                    source_.deliverUndelivered(std::type_index(typeid(EventT)),
                                               static_cast<Event*>(evt.release()));
                    return ref;
                }

                auto typeIdx = std::type_index(typeid(EventT));

                if (reg.isThreadedMode() && dest->getCore() != source_.getCore()) {
                    // Cross-core: enqueue to destination core's mailbox
                    Mailbox* mbox = reg.getMailbox(dest->getCore());
                    if (mbox) {
                        Actor* destPtr = dest;
                        Event* rawEvt = evt.release();
                        mbox->enqueue([destPtr, typeIdx, rawEvt]() {
                            destPtr->deliverEvent(typeIdx, rawEvt);
                        });
                    } else {
                        dest->deliverEvent(typeIdx, static_cast<Event*>(evt.release()));
                    }
                } else {
                    // Same core or synchronous mode: deliver inline
                    dest->deliverEvent(typeIdx, static_cast<Event*>(evt.release()));
                }

                return ref;
            }

            void setDestinationActorId(const ActorId& id) { destId_ = id; }
            const ActorId& getDestinationActorId() const { return destId_; }

        private:
            Actor& source_;
            ActorId destId_;
        };

        class Batch {
        public:
            Batch(Pipe&) {}
            bool isPushCommitted(Pipe&) { return true; }
        };
    };

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

    template<typename ActorT, typename... Args>
    ActorId newUnreferencedActor(Args&&... args) {
        auto actor = std::make_unique<ActorT>(std::forward<Args>(args)...);
        ActorId id = actor->getActorId();
        ownedActors_.push_back(std::move(actor));
        return id;
    }

    template<typename ActorT, typename... Args>
    std::shared_ptr<ActorT> newReferencedActor(Args&&... args) {
        return std::make_shared<ActorT>(std::forward<Args>(args)...);
    }

    void requestDestroy() { destroyRequested_ = true; }

    class ServiceIndex {
    public:
        template<typename ServiceTag>
        const ActorId& getServiceActorId() const {
            static ActorId id = ActorRegistry::instance()
                .getService(std::type_index(typeid(ServiceTag)));
            return id;
        }
    };

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
        for (auto* cb : cbs) cb->onCallback();
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

// ── Engine — multi-threaded actor scheduler ────────────────────────
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
            int core = coreId;
            factories_.push_back([core, ... a = std::forward<Args>(args)]() mutable {
                auto actor = std::make_unique<ActorT>(std::move(a)...);
                actor->actorId_.coreId = static_cast<uint8_t>(core);
                return std::make_pair(core, std::move(actor));
            });
        }

        template<typename ServiceTag, typename ActorT, typename... Args>
        void addServiceActor(int coreId, Args&&... args) {
            int core = coreId;
            factories_.push_back([core, ... a = std::forward<Args>(args)]() mutable {
                auto actor = std::make_unique<ActorT>(std::move(a)...);
                actor->actorId_.coreId = static_cast<uint8_t>(core);
                ActorRegistry::instance().registerService(
                    std::type_index(typeid(ServiceTag)), actor->getActorId());
                return std::make_pair(core, std::move(actor));
            });
        }

        void setRedZoneCore(int) {}
        void setBlueZoneCore(int) {}

        using Factory = std::function<std::pair<int, std::unique_ptr<Actor>>()>;
        std::vector<Factory> factories_;
    };

    explicit Engine(StartSequence& seq) {
        for (auto& factory : seq.factories_) {
            auto result = factory();
            coreActors_[result.first].push_back(std::move(result.second));
        }

        for (auto it = coreActors_.begin(); it != coreActors_.end(); ++it) {
            auto mbox = std::make_unique<Mailbox>();
            ActorRegistry::instance().registerMailbox(
                static_cast<uint8_t>(it->first), mbox.get());
            mailboxes_[it->first] = std::move(mbox);
        }

        running_ = true;
    }

    ~Engine() {
        stop();
        for (auto& t : threads_) {
            if (t.joinable()) t.join();
        }
        ActorRegistry::instance().setThreadedMode(false);
        for (auto it = mailboxes_.begin(); it != mailboxes_.end(); ++it) {
            ActorRegistry::instance().unregisterMailbox(static_cast<uint8_t>(it->first));
        }
        for (auto it = coreActors_.rbegin(); it != coreActors_.rend(); ++it) {
            while (!it->second.empty()) it->second.pop_back();
        }
    }

    void runFor(std::chrono::milliseconds duration) {
        ActorRegistry::instance().setThreadedMode(true);

        auto deadline = std::chrono::steady_clock::now() + duration;

        for (auto it = coreActors_.begin(); it != coreActors_.end(); ++it) {
            int coreId = it->first;
            Mailbox* mbox = mailboxes_[coreId].get();
            auto* actorList = &it->second;
            threads_.emplace_back([mbox, actorList, deadline, this]() {
                while (running_ && std::chrono::steady_clock::now() < deadline) {
                    mbox->waitAndDrain(std::chrono::milliseconds(5));
                    for (auto& actor : *actorList) {
                        actor->processPendingCallbacks();
                    }
                }
            });

#ifdef __linux__
            if (threads_.back().joinable()) {
                cpu_set_t cpuset;
                CPU_ZERO(&cpuset);
                CPU_SET(coreId, &cpuset);
                pthread_setaffinity_np(threads_.back().native_handle(),
                                       sizeof(cpu_set_t), &cpuset);
            }
#endif
        }

        std::this_thread::sleep_for(duration);
        stop();

        for (auto& t : threads_) {
            if (t.joinable()) t.join();
        }
        threads_.clear();
        ActorRegistry::instance().setThreadedMode(false);
    }

    void stop() {
        running_ = false;
        for (auto it = mailboxes_.begin(); it != mailboxes_.end(); ++it) {
            it->second->shutdown();
        }
    }

    bool isRunning() const { return running_; }

    size_t coreCount() const { return coreActors_.size(); }

    void drain() {
        for (auto it = mailboxes_.begin(); it != mailboxes_.end(); ++it) {
            it->second->drainAll();
        }
        for (auto it = coreActors_.begin(); it != coreActors_.end(); ++it) {
            for (auto& actor : it->second) {
                actor->processPendingCallbacks();
            }
        }
    }

private:
    std::map<int, std::vector<std::unique_ptr<Actor>>> coreActors_;
    std::map<int, std::unique_ptr<Mailbox>> mailboxes_;
    std::vector<std::thread> threads_;
    std::atomic<bool> running_{false};
};

} // namespace tredzone

#endif // EUNEX_REAL_SIMPLX
