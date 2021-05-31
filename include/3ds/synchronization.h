/**
 * @file synchronization.h
 * @brief Provides synchronization locks.
 */
#pragma once
#include <3ds/svc.h>

/// A light lock.
typedef s32 LightLock;

/// Performs a Data Synchronization Barrier operation.
static inline void __dsb(void)
{
	__asm__ __volatile__("mcr p15, 0, %[val], c7, c10, 4" :: [val] "r" (0) : "memory");
}

/// Performs a Data Memory Barrier operation.
static inline void __dmb(void)
{
	__asm__ __volatile__("mcr p15, 0, %[val], c7, c10, 5" :: [val] "r" (0) : "memory");
}

/// Performs a clrex operation.
static inline void __clrex(void)
{
	__asm__ __volatile__("clrex" ::: "memory");
}

/**
 * @brief Performs a ldrex operation.
 * @param addr Address to perform the operation on.
 * @return The resulting value.
 */
static inline s32 __ldrex(s32* addr)
{
	s32 val;
	__asm__ __volatile__("ldrex %[val], %[addr]" : [val] "=r" (val) : [addr] "Q" (*addr));
	return val;
}

/**
 * @brief Performs a strex operation.
 * @param addr Address to perform the operation on.
 * @param val Value to store.
 * @return Whether the operation was successful.
 */
static inline bool __strex(s32* addr, s32 val)
{
	bool res;
	__asm__ __volatile__("strex %[res], %[val], %[addr]" : [res] "=&r" (res) : [val] "r" (val), [addr] "Q" (*addr));
	return res;
}

/**
 * @brief Function used to implement user-mode synchronization primitives.
 * @param addr Pointer to a signed 32-bit value whose address will be used to identify waiting threads.
 * @param type Type of action to be performed by the arbiter
 * @param value Number of threads to signal if using @ref ARBITRATION_SIGNAL, or the value used for comparison.
 *
 * This will perform an arbitration based on #type. The comparisons are done between #value and the value at the address #addr.
 *
 * @code
 * s32 val=0;
 * // Does *nothing* since val >= 0
 * syncArbitrateAddress(&val,ARBITRATION_WAIT_IF_LESS_THAN,0);
 * @endcode
 *
 * @note Usage of this function entails an implicit Data Memory Barrier (dmb).
 */
Result syncArbitrateAddress(s32* addr, ArbitrationType type, s32 value);

/**
 * @brief Initializes a light lock.
 * @param lock Pointer to the lock.
 */
void LightLock_Init(LightLock* lock);

/**
 * @brief Locks a light lock.
 * @param lock Pointer to the lock.
 */
void LightLock_Lock(LightLock* lock);

/**
 * @brief Unlocks a light lock.
 * @param lock Pointer to the lock.
 */
void LightLock_Unlock(LightLock* lock);

#define LIGHTLOCK_STATICINIT ((LightLock)1)
