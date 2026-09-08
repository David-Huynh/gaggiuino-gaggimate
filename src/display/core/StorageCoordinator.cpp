#include "StorageCoordinator.h"

#include <cassert>
#include <utility>
#if defined(ARDUINO_ARCH_ESP32)
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

StorageCoordinator::FlashOwner StorageCoordinator::currentFlashOwner() {
#if defined(ARDUINO_ARCH_ESP32)
    // Settings acquires flash during global construction, before the scheduler
    // starts. Later callers include native Arduino/FreeRTOS tasks, which also
    // lack a pthread identity. pthread_self()/std::this_thread::get_id() assert
    // on both paths in ESP-IDF. Global constructors run serially on CPU0.
    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED)
        return nullptr;
    const TaskHandle_t task = xTaskGetCurrentTaskHandle();
    assert(task != nullptr);
    return task;
#else
    return std::this_thread::get_id();
#endif
}

StorageCoordinator::ProcessLease::ProcessLease(ProcessLease &&other) noexcept
    : owner(std::exchange(other.owner, nullptr)) {}

StorageCoordinator::ProcessLease &StorageCoordinator::ProcessLease::operator=(ProcessLease &&other) noexcept {
    if (this != &other) {
        reset();
        owner = std::exchange(other.owner, nullptr);
    }
    return *this;
}

StorageCoordinator::ProcessLease::~ProcessLease() { reset(); }

void StorageCoordinator::ProcessLease::reset() {
    if (owner) {
        StorageCoordinator *coordinator = std::exchange(owner, nullptr);
        coordinator->releaseProcess();
    }
}

StorageCoordinator::FlashLease::FlashLease(FlashLease &&other) noexcept
    : owner(std::exchange(other.owner, nullptr)) {}

StorageCoordinator::FlashLease &StorageCoordinator::FlashLease::operator=(FlashLease &&other) noexcept {
    if (this != &other) {
        reset();
        owner = std::exchange(other.owner, nullptr);
    }
    return *this;
}

StorageCoordinator::FlashLease::~FlashLease() { reset(); }

void StorageCoordinator::FlashLease::reset() {
    if (owner) {
        StorageCoordinator *coordinator = std::exchange(owner, nullptr);
        coordinator->releaseFlash();
    }
}

bool StorageCoordinator::FlashLease::checkpoint() {
    if (!owner) {
        return false;
    }
    StorageCoordinator *coordinator = owner;
    reset();
    *this = coordinator->acquireFlash();
    return owner != nullptr;
}

StorageCoordinator &StorageCoordinator::instance() {
    static StorageCoordinator coordinator;
    return coordinator;
}

StorageCoordinator::ProcessLease StorageCoordinator::acquireProcess() {
    std::unique_lock<std::mutex> lock(mutex);
    if (processOwned) {
        return {};
    }
    ++processWaiters;
    changed.wait(lock, [this] { return !flashOwned && !processOwned; });
    --processWaiters;
    processOwned = true;
    ++processEpoch;
    return ProcessLease(this);
}

StorageCoordinator::FlashLease StorageCoordinator::acquireFlash() {
    std::unique_lock<std::mutex> lock(mutex);
    const FlashOwner caller = currentFlashOwner();
    if (flashOwned && flashOwner == caller) {
        ++flashDepth;
        return FlashLease(this);
    }
    changed.wait(lock, [this] { return !flashOwned && !processOwned && processWaiters == 0; });
    flashOwned = true;
    flashOwner = caller;
    flashDepth = 1;
    return FlashLease(this);
}

StorageCoordinator::FlashLease StorageCoordinator::tryAcquireFlash() {
    std::lock_guard<std::mutex> lock(mutex);
    const FlashOwner caller = currentFlashOwner();
    if (flashOwned && flashOwner == caller) {
        ++flashDepth;
        return FlashLease(this);
    }
    if (flashOwned || processOwned || processWaiters > 0) {
        return {};
    }
    flashOwned = true;
    flashOwner = caller;
    flashDepth = 1;
    return FlashLease(this);
}

bool StorageCoordinator::processActive() const {
    std::lock_guard<std::mutex> lock(mutex);
    return processOwned;
}

bool StorageCoordinator::processPending() const {
    std::lock_guard<std::mutex> lock(mutex);
    return processWaiters > 0;
}

std::uint64_t StorageCoordinator::processGeneration() const {
    std::lock_guard<std::mutex> lock(mutex);
    return processEpoch;
}

bool StorageCoordinator::flashActive() const {
    std::lock_guard<std::mutex> lock(mutex);
    return flashOwned;
}

bool StorageCoordinator::currentThreadOwnsFlash() const {
    std::lock_guard<std::mutex> lock(mutex);
    return flashOwned && flashOwner == currentFlashOwner();
}

void StorageCoordinator::assertFlashLease() const { assert(currentThreadOwnsFlash()); }

void StorageCoordinator::releaseProcess() {
    {
        std::lock_guard<std::mutex> lock(mutex);
        assert(processOwned);
        processOwned = false;
    }
    changed.notify_all();
}

void StorageCoordinator::releaseFlash() {
    {
        std::lock_guard<std::mutex> lock(mutex);
        assert(flashOwned && flashOwner == currentFlashOwner() && flashDepth > 0);
        if (--flashDepth > 0) {
            return;
        }
        flashOwned = false;
        flashOwner = FlashOwner{};
    }
    changed.notify_all();
}
