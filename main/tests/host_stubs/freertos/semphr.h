/*
 * Host stub for freertos/semphr.h.
 *
 * Backed by a real std::mutex, so concurrency tests actually contend: the
 * point of testing auth_manager's integration-token mutex is that two threads
 * really do race for it. A do-nothing stub would let every interleaving pass.
 *
 * It also tracks the owning thread and take/give counts, so double-take and
 * give-without-take are detectable rather than silently fine. Those are the
 * mistakes that deadlock or corrupt state on the device and are invisible in a
 * stub that does nothing.
 */
#pragma once

#include "FreeRTOS.h"

#include <atomic>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>

// Counters aggregated over every mutex. auth_manager's integration-token
// mutex is a file-scope static with no accessor, so a test cannot name the
// handle; these totals are how it asserts "the failure path gave the lock
// back" without reaching into the module.
namespace sem_fake {
inline std::atomic<int> &total_takes() {
    static std::atomic<int> v{0};
    return v;
}
inline std::atomic<int> &total_gives() {
    static std::atomic<int> v{0};
    return v;
}
inline std::atomic<int> &total_contended() {
    static std::atomic<int> v{0};
    return v;
}
inline void reset_totals() {
    total_takes().store(0);
    total_gives().store(0);
    total_contended().store(0);
}
}  // namespace sem_fake

struct sem_impl {
    std::mutex m;
    // Owner is read by a thread that does not hold the lock (to detect a
    // recursive take), so it has to be atomic rather than a plain member.
    std::atomic<bool> held{false};
    std::atomic<std::size_t> owner{0};
    std::atomic<int> takes{0};
    std::atomic<int> gives{0};
    std::atomic<int> contended{0};
};

inline std::size_t sem_this_thread_id() {
    return std::hash<std::thread::id>{}(std::this_thread::get_id());
}

typedef sem_impl *SemaphoreHandle_t;

// Storage for the *Static variants. The host has no static-allocation
// constraint, so the buffer is only checked for being non-null.
typedef struct {
    void *placeholder;
} StaticSemaphore_t;

inline SemaphoreHandle_t xSemaphoreCreateMutex(void) { return new sem_impl(); }

inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *buffer) {
    if (buffer == nullptr) {
        return nullptr;
    }
    return new sem_impl();
}

inline void vSemaphoreDelete(SemaphoreHandle_t s) { delete s; }

inline BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks) {
    (void)ticks;
    if (!s) {
        return pdFALSE;
    }
    // A recursive take on a plain FreeRTOS mutex blocks forever on the device.
    // Report failure instead of hanging the test run.
    if (s->held.load() && s->owner.load() == sem_this_thread_id()) {
        return pdFALSE;
    }
    if (!s->m.try_lock()) {
        s->contended.fetch_add(1);
        sem_fake::total_contended().fetch_add(1);
        s->m.lock();
    }
    s->held.store(true);
    s->owner.store(sem_this_thread_id());
    s->takes.fetch_add(1);
    sem_fake::total_takes().fetch_add(1);
    return pdTRUE;
}

inline BaseType_t xSemaphoreGive(SemaphoreHandle_t s) {
    if (!s || !s->held.load()) {
        return pdFALSE;
    }
    s->held.store(false);
    s->owner.store(0);
    s->gives.fetch_add(1);
    sem_fake::total_gives().fetch_add(1);
    s->m.unlock();
    return pdTRUE;
}

namespace sem_fake {

inline int takes(SemaphoreHandle_t s) { return s ? s->takes.load() : 0; }
inline int gives(SemaphoreHandle_t s) { return s ? s->gives.load() : 0; }
// How often a take had to wait. A concurrency test that never contends is not
// testing concurrency.
inline int contended(SemaphoreHandle_t s) { return s ? s->contended.load() : 0; }

}  // namespace sem_fake
