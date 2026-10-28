#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <memory>
#include "unicorn/unicorn.h"

namespace emux {

struct GuestThread {
    uint32_t tid{0};
    uint32_t startRoutine{0};
    uint32_t arg{0};
    uint32_t stackBase{0};
    uint32_t stackSize{0};
    uint32_t stackTop{0};
    uint32_t tlsAddr{0};
    uint32_t returnValue{0};

    std::atomic<bool> isRunning{false};
    std::atomic<bool> isFinished{false};
    std::atomic<bool> isDetached{false};

    std::unique_ptr<std::thread> hostThread;
    std::mutex mutex;
    std::condition_variable cv;

    uc_engine* uc{nullptr};
    std::unordered_map<uint32_t, uint32_t> tlsKeyValues;
};

class GuestThreadScheduler {
public:
    static GuestThreadScheduler& getInstance();

    void initialize(uc_engine* mainUc);
    void shutdown();

    int32_t createThread(
        uint32_t threadOutPtr,
        uint32_t attrPtr,
        uint32_t startRoutine,
        uint32_t arg,
        uc_engine* callerUc
    );

    int32_t joinThread(uint32_t tid, uint32_t retvalPtr, uc_engine* callerUc);
    int32_t detachThread(uint32_t tid);
    uint32_t getCurrentTid();
    void exitThread(uint32_t retval, uc_engine* callerUc);

    uint32_t createTlsKey(uint32_t destructorFn);
    int32_t deleteTlsKey(uint32_t key);
    int32_t setTlsSpecific(uint32_t key, uint32_t val);
    uint32_t getTlsSpecific(uint32_t key);

    size_t getActiveThreadCount() const;

private:
    GuestThreadScheduler() = default;
    ~GuestThreadScheduler();

    void threadWorkerLoop(std::shared_ptr<GuestThread> thread);

    mutable std::mutex schedulerMutex_;
    std::unordered_map<uint32_t, std::shared_ptr<GuestThread>> threads_;
    std::atomic<uint32_t> nextTid_{2};

    std::atomic<uint32_t> nextTlsKey_{1};
    std::unordered_map<uint32_t, uint32_t> tlsKeyDestructors_;

    uc_engine* mainUc_{nullptr};
    std::atomic<bool> isShutdown_{false};
};

} // namespace emux
