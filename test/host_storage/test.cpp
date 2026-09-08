#include "StorageCoordinator.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <future>
#include <thread>
#include <utility>

#if defined(ARDUINO_ARCH_ESP32)
#include <freertos/task.h>
static std::atomic<BaseType_t> schedulerState{taskSCHEDULER_NOT_STARTED};
static std::atomic<unsigned> taskQueries{0};
static thread_local int taskToken;
static thread_local TaskHandle_t currentTask = &taskToken;
BaseType_t xTaskGetSchedulerState() { return schedulerState.load(); }
TaskHandle_t xTaskGetCurrentTaskHandle() {
    assert(schedulerState.load() != taskSCHEDULER_NOT_STARTED);
    ++taskQueries;
    return currentTask;
}
#endif

// Match Settings: acquire/release during global construction, before main.
static bool bootPassed = false;
struct BootProbe {
    BootProbe() {
        auto &storage = StorageCoordinator::instance();
        assert(!storage.currentThreadOwnsFlash());
        {
            auto outer = storage.acquireFlash();
            auto inner = storage.tryAcquireFlash();
            assert(outer && inner && storage.currentThreadOwnsFlash());
            storage.assertFlashLease();
            inner.reset();
            assert(storage.flashActive());
        }
        assert(!storage.flashActive());
#if defined(ARDUINO_ARCH_ESP32)
        assert(taskQueries.load() == 0);
#endif
        bootPassed = true;
    }
} bootProbe;

static void testNesting() {
    auto &storage = StorageCoordinator::instance();
    auto outer = storage.acquireFlash();
    auto inner = storage.acquireFlash();
    auto moved = std::move(inner);
    assert(!inner && moved);
    outer.reset();
    assert(storage.flashActive() && storage.currentThreadOwnsFlash());
    assert(moved.checkpoint());
    moved.reset();
    assert(!storage.flashActive() && !storage.currentThreadOwnsFlash());
    assert(!moved.checkpoint());
}

#if defined(ARDUINO_ARCH_ESP32)
static void testNativeTaskIdentity(bool wrongOwner) {
    auto &storage = StorageCoordinator::instance();
    auto ownerTask = currentTask;
    auto flash = storage.acquireFlash();
    int otherTask;
    // Two FreeRTOS tasks on the same host thread: a pthread ID cannot distinguish them.
    currentTask = &otherTask;
    if (wrongOwner) {
        flash.reset(); // must assert, never release another task's lease
        assert(false && "Wrong task released a flash lease");
    }
    assert(!storage.currentThreadOwnsFlash());
    assert(!storage.tryAcquireFlash());
    schedulerState = taskSCHEDULER_SUSPENDED;
    assert(!storage.tryAcquireFlash()); // suspended is not pre-scheduler boot
    currentTask = ownerTask;
    storage.assertFlashLease();
    schedulerState = taskSCHEDULER_RUNNING;
    flash.reset();
}
#endif

static void testExclusionAndWakeup() {
    auto &storage = StorageCoordinator::instance();
    auto flash = storage.acquireFlash();
    std::promise<void> processStarted, finishProcess;
    auto started = processStarted.get_future();
    auto finish = finishProcess.get_future();
    std::thread processTask([&] {
        assert(!storage.currentThreadOwnsFlash());
        assert(!storage.tryAcquireFlash());
        auto process = storage.acquireProcess();
        assert(process);
        processStarted.set_value();
        finish.wait();
    });
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!storage.processPending() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    assert(storage.processPending());
    assert(!storage.processActive());
    flash.reset();
    assert(started.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    assert(storage.processActive() && !storage.tryAcquireFlash());
    assert(!storage.acquireProcess()); // duplicate start cannot take ownership
    finishProcess.set_value();
    processTask.join();
    assert(!storage.processActive() && !storage.processPending());
    assert(storage.tryAcquireFlash());
}

int main(int argc, char **) {
    assert(bootPassed);
#if defined(ARDUINO_ARCH_ESP32)
    schedulerState = taskSCHEDULER_RUNNING;
    testNativeTaskIdentity(argc > 1);
    assert(taskQueries.load() > 0);
#endif
    testNesting();
    testExclusionAndWakeup();
    puts("PASS storage: global construction, nested/moved leases, owner isolation, process exclusion and wakeup");
}
