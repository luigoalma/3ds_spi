#include <string.h>
#include <3ds/types.h>
#include <3ds/svc.h>
#include <3ds/result.h>
#include <3ds/synchronization.h>

static Handle arbiter;

Result __sync_init(void)
{
	return svcCreateAddressArbiter(&arbiter);
}

void __sync_fini(void)
{
	if (arbiter)
		svcCloseHandle(arbiter);
}

Result syncArbitrateAddress(s32* addr, ArbitrationType type, s32 value)
{
	return svcArbitrateAddress(arbiter, (u32)addr, type, value, 0LL);
}

void LightLock_Init(LightLock* lock)
{
	do
		__ldrex(lock);
	while (__strex(lock, 1));
}

void LightLock_Lock(LightLock* lock)
{
	s32 val;
	bool bAlreadyLocked;

	// Try to lock, or if that's not possible, increment the number of waiting threads
	do
	{
		// Read the current lock state
		val = __ldrex(lock);
		if (val == 0) val = 1; // 0 is an invalid state - treat it as 1 (unlocked)
		bAlreadyLocked = val < 0;

		// Calculate the desired next state of the lock
		if (!bAlreadyLocked)
			val = -val; // transition into locked state
		else
			--val; // increment the number of waiting threads (which has the sign reversed during locked state)
	} while (__strex(lock, val));

	// While the lock is held by a different thread:
	while (bAlreadyLocked)
	{
		// Wait for the lock holder thread to wake us up
		syncArbitrateAddress(lock, ARBITRATION_WAIT_IF_LESS_THAN, 0);

		// Try to lock again
		do
		{
			// Read the current lock state
			val = __ldrex(lock);
			bAlreadyLocked = val < 0;

			// Calculate the desired next state of the lock
			if (!bAlreadyLocked)
				val = -(val-1); // decrement the number of waiting threads *and* transition into locked state
			else
			{
				// Since the lock is still held, we need to cancel the atomic update and wait again
				__clrex();
				break;
			}
		} while (__strex(lock, val));
	}

	__dmb();
}

void LightLock_Unlock(LightLock* lock)
{
	__dmb();

	s32 val;
	do
		val = -__ldrex(lock);
	while (__strex(lock, val));

	if (val > 1)
		// Wake up exactly one thread
		syncArbitrateAddress(lock, ARBITRATION_SIGNAL, 1);
}
