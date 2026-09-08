#ifndef STORAGECOORDINATOR_H
#define STORAGECOORDINATOR_H

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#if !defined(ARDUINO_ARCH_ESP32)
#include <thread>
#endif

class StorageCoordinator {
  public:
    static constexpr std::size_t MAX_FLASH_QUANTUM_BYTES = 4096;

    class ProcessLease {
      public:
        ProcessLease() = default;
        ProcessLease(ProcessLease const &) = delete;
        ProcessLease &operator=(ProcessLease const &) = delete;
        ProcessLease(ProcessLease &&other) noexcept;
        ProcessLease &operator=(ProcessLease &&other) noexcept;
        ~ProcessLease();

        explicit operator bool() const { return owner != nullptr; }
        void reset();

      private:
        friend class StorageCoordinator;
        explicit ProcessLease(StorageCoordinator *coordinator) : owner(coordinator) {}
        StorageCoordinator *owner = nullptr;
    };

    class FlashLease {
      public:
        FlashLease() = default;
        FlashLease(FlashLease const &) = delete;
        FlashLease &operator=(FlashLease const &) = delete;
        FlashLease(FlashLease &&other) noexcept;
        FlashLease &operator=(FlashLease &&other) noexcept;
        ~FlashLease();

        explicit operator bool() const { return owner != nullptr; }
        void reset();
        bool checkpoint();

      private:
        friend class StorageCoordinator;
        explicit FlashLease(StorageCoordinator *coordinator) : owner(coordinator) {}
        StorageCoordinator *owner = nullptr;
    };

    static StorageCoordinator &instance();

    // Process waiters close the flash gate before waiting. A duplicate process
    // request returns immediately instead of blocking behind the active process.
    ProcessLease acquireProcess();
    FlashLease acquireFlash();
    FlashLease tryAcquireFlash();

    bool processActive() const;
    bool processPending() const;
    std::uint64_t processGeneration() const;
    bool flashActive() const;
    bool currentThreadOwnsFlash() const;
    void assertFlashLease() const;

  private:
#if defined(ARDUINO_ARCH_ESP32)
    using FlashOwner = void *; // FreeRTOS task handle; nullptr during global construction
#else
    using FlashOwner = std::thread::id;
#endif
    static FlashOwner currentFlashOwner();
    void releaseProcess();
    void releaseFlash();

    mutable std::mutex mutex;
    std::condition_variable changed;
    bool processOwned = false;
    unsigned processWaiters = 0;
    std::uint64_t processEpoch = 0;
    bool flashOwned = false;
    FlashOwner flashOwner{};
    unsigned flashDepth = 0;
};

#endif // STORAGECOORDINATOR_H
