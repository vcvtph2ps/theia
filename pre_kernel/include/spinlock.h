#pragma once
#include <lib/helpers.h>
#include <stdint.h>

/**
 * @brief A spinlock that disables preemption on acquire and restores it on release.
 * @note These should be used when the lock cannot be grabbed from deferred work or hardirqs
 */
typedef struct {
    ATOMIC uint32_t lock;
} spinlock_t;

#define SPINLOCK_INIT ((spinlock_t) { 0 })

/**
 * @brief Acquires a spinlock and increments the preemption counter
 * @param lock Pointer to the spinlock to acquire.
 */
void spinlock_lock(spinlock_t* lock);

/**
 * @brief Releases a spinlock and decrements the preemption counter
 * @param lock Pointer to the spinlock to release.
 */
void spinlock_unlock(spinlock_t* lock);
