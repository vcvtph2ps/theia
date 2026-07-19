#include <lib/helpers.h>
#include <spinlock.h>
#include <stdint.h>

[[nodiscard]] static inline bool spinlock_try_lock(ATOMIC_PARAM uint32_t* lock) {
    return !ATOMIC_XCHG(lock, 1, ATOMIC_ACQUIRE);
}

static inline void spinlock_unlock_raw(ATOMIC_PARAM uint32_t* lock) {
    ATOMIC_STORE(lock, 0, ATOMIC_RELEASE);
}

static inline void spinlock_lock_raw(ATOMIC_PARAM uint32_t* lock) {
    while(true) {
        if(spinlock_try_lock(lock)) return;
        while(ATOMIC_LOAD(lock, ATOMIC_RELAXED)) {}
    }
}

void spinlock_lock(spinlock_t* lock) {
    spinlock_lock_raw(&lock->lock);
}

void spinlock_unlock(spinlock_t* lock) {
    spinlock_unlock_raw(&lock->lock);
}
