#pragma once

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"

typedef portMUX_TYPE coupe_lock_t;
#define COUPE_LOCK_INIT portMUX_INITIALIZER_UNLOCKED

static inline void coupe_lock(coupe_lock_t *lock)
{
    portENTER_CRITICAL(lock);
}

static inline void coupe_unlock(coupe_lock_t *lock)
{
    portEXIT_CRITICAL(lock);
}
#else
#include <stdatomic.h>

typedef atomic_flag coupe_lock_t;
#define COUPE_LOCK_INIT ATOMIC_FLAG_INIT

static inline void coupe_lock(coupe_lock_t *lock)
{
    while (atomic_flag_test_and_set_explicit(lock, memory_order_acquire)) {
    }
}

static inline void coupe_unlock(coupe_lock_t *lock)
{
    atomic_flag_clear_explicit(lock, memory_order_release);
}
#endif
