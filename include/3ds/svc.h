/**
 * @file svc.h
 * @brief Syscall wrappers.
 */
#pragma once

#include "types.h"

/// Arbitration modes.
typedef enum {
	ARBITRATION_SIGNAL                                  = 0, ///< Signal #value threads for wake-up.
	ARBITRATION_WAIT_IF_LESS_THAN                       = 1, ///< If the memory at the address is strictly lower than #value, then wait for signal.
	ARBITRATION_DECREMENT_AND_WAIT_IF_LESS_THAN         = 2, ///< If the memory at the address is strictly lower than #value, then decrement it and wait for signal.
	ARBITRATION_WAIT_IF_LESS_THAN_TIMEOUT               = 3, ///< If the memory at the address is strictly lower than #value, then wait for signal or timeout.
	ARBITRATION_DECREMENT_AND_WAIT_IF_LESS_THAN_TIMEOUT = 4, ///< If the memory at the address is strictly lower than #value, then decrement it and wait for signal or timeout.
} ArbitrationType;

/// Reasons for a user break.
typedef enum {
	USERBREAK_PANIC         = 0, ///< Panic.
	USERBREAK_ASSERT        = 1, ///< Assertion failed.
	USERBREAK_USER          = 2, ///< User related.
	USERBREAK_LOAD_RO       = 3, ///< Load RO.
	USERBREAK_UNLOAD_RO     = 4, ///< Unload RO.
} UserBreakType;

/**
 * @brief Gets the thread local storage buffer.
 * @return The thread local storage bufger.
 */
static inline void* getThreadLocalStorage(void)
{
	void* ret;
	__asm__ ("mrc p15, 0, %[data], c13, c0, 3" : [data] "=r" (ret));
	return ret;
}

/**
 * @brief Gets the thread command buffer.
 * @return The thread command bufger.
 */
static inline u32* getThreadCommandBuffer(void)
{
	return (u32*)((u8*)getThreadLocalStorage() + 0x80);
}

/**
 * @brief Gets the thread static buffer.
 * @return The thread static bufger.
 */
static inline u32* getThreadStaticBuffers(void)
{
	return (u32*)((u8*)getThreadLocalStorage() + 0x180);
}

/**
 * @brief Gets the ID of a process.
 * @param[out] out Pointer to output the process ID to.
 * @param handle Handle of the process to get the ID of.
 */
static inline Result svcGetProcessId(u32 *out, Handle handle) {
	register const Handle _handle __asm__("r1") = handle;

	register Result res __asm__("r0");
	register Handle out_handle __asm__("r1");

	__asm__ volatile ("svc\t0x35" : "=r"(res), "=r"(out_handle) : "r"(_handle) : "r2", "r3", "r12");

	*out = out_handle;

	return res;
}

/**
 * @brief Connects to a port.
 * @param[out] out Pointer to output the port handle to.
 * @param portName Name of the port.
 */
static inline Result svcConnectToPort(volatile Handle* out, const char* portName) {
	register const char* _portName __asm__("r1") = portName;

	register Result res __asm__("r0");
	register Handle out_handle __asm__("r1");

	__asm__ volatile ("svc\t0x2D" : "=r"(res), "=r"(out_handle) : "r"(_portName) : "r2", "r3", "r12", "memory");

	*out = out_handle;

	return res;
}

/**
 * @brief Creates a new thread.
 * @param[out] thread     The thread handle
 * @param entrypoint      The function that will be called first upon thread creation
 * @param arg             The argument passed to @p entrypoint
 * @param stack_top       The top of the thread's stack. Must be 0x8 bytes mem-aligned.
 * @param thread_priority Low values gives the thread higher priority.
 *                        For userland apps, this has to be within the range [0x18;0x3F]
 * @param processor_id    The id of the processor the thread should be ran on. Those are labelled starting from 0.
 *                        For old 3ds it has to be <2, and for new 3DS <4.
 *                        Value -1 means all CPUs and -2 read from the Exheader.
 *
 * The processor with ID 1 is the system processor.
 * To enable multi-threading on this core you need to call APT_SetAppCpuTimeLimit at least once with a non-zero value.
 *
 * Since a thread is considered as a waitable object, you can use @ref svcWaitSynchronization
 * and @ref svcWaitSynchronizationN to join with it.
 *
 * @note The kernel will clear the @p stack_top's address low 3 bits to make sure it is 0x8-bytes aligned.
 */
static inline Result svcCreateThread(Handle* thread, ThreadFunc entrypoint, u32 arg, u32* stack_top, s32 thread_priority, s32 processor_id) {
	register const s32 _thread_priority __asm__("r0") = thread_priority;
	register const ThreadFunc _entrypoint __asm__("r1") = entrypoint;
	register u32 _arg __asm__("r2") = arg;
	register u32* _stack_top __asm__("r3") = stack_top;
	register const s32 _processor_id __asm__("r4") = processor_id;

	register Result res __asm__("r0");
	register Handle out_thread __asm__("r1");

	__asm__ volatile ("svc\t0x08" : "=r"(res), "=r"(out_thread), "+r"(_arg), "+r"(_stack_top) : "r"(_thread_priority), "r"(_entrypoint), "r"(_processor_id) : "r12", "memory");

	*thread = out_thread;

	return res;
}

/**
 * @brief Puts the current thread to sleep.
 * @param ns The minimum number of nanoseconds to sleep for.
 */
static inline void svcSleepThread(s64 ns) {
	register u32 lo_ns __asm__("r0") = (u32)(((u64)ns) & ((u32)~0));
	register u32 hi_ns __asm__("r1") = (u32)(((u64)ns) >> 32);

	__asm__ volatile ("svc\t0x0A" : "+r"(lo_ns), "+r"(hi_ns) : : "r2", "r3", "r12");
}

/**
 * @brief Waits for synchronization on a handle.
 * @param handle Handle to wait on.
 * @param nanoseconds Maximum nanoseconds to wait for.
 */
static inline Result svcWaitSynchronization(Handle handle, s64 nanoseconds) {
	register const Handle _handle __asm__("r0") = handle;
	register u32 lo_ns __asm__("r2") = (u32)(((u64)nanoseconds) & ((u32)~0));
	register u32 hi_ns __asm__("r3") = (u32)(((u64)nanoseconds) >> 32);

	register Result res __asm__("r0");

	__asm__ volatile ("svc\t0x24" : "=r"(res), "+r"(lo_ns), "+r"(hi_ns) : "r"(_handle) : "r1", "r12", "memory");

	return res;
}

/**
 * @brief Waits for synchronization on multiple handles.
 * @param[out] out Pointer to output the index of the synchronized handle to.
 * @param handles Handles to wait on.
 * @param handles_num Number of handles.
 * @param wait_all Whether to wait for synchronization on all handles.
 * @param nanoseconds Maximum nanoseconds to wait for.
 */
static inline Result svcWaitSynchronizationN(s32* out, const Handle* handles, s32 handles_num, bool wait_all, s64 nanoseconds) {
	register const Handle* _handles __asm__("r1") = handles;
	register s32 _handles_num __asm__("r2") = handles_num;
	register bool _wait_all __asm__("r3") = wait_all;
	register const u32 lo_ns __asm__("r0") = (u32)(((u64)nanoseconds) & ((u32)~0));
	register const u32 hi_ns __asm__("r4") = (u32)(((u64)nanoseconds) >> 32);

	register s32 _out_value __asm__("r1");
	register Result res __asm__("r0");

	__asm__ volatile ("svc\t0x25" : "=r"(res), "=r"(_out_value), "+r"(_handles_num), "+r"(_wait_all) : "r"(lo_ns), "r"(_handles), "r"(hi_ns) : "r12", "memory");

	*out = _out_value;

	return res;
}

/**
 * @brief Creates an address arbiter
 * @param[out] mutex Pointer to output the handle of the created address arbiter to.
 * @sa svcArbitrateAddress
 */
static inline Result svcCreateAddressArbiter(Handle *arbiter) {
	register Result res __asm__("r0");
	register s32 out_handle __asm__("r1");

	__asm__ volatile ("svc\t0x21" : "=r"(res), "=r"(out_handle) : : "r2", "r3", "r12", "memory");

	*arbiter = out_handle;

	return res;
}

/**
 * @brief Arbitrate an address, can be used for synchronization
 * @param arbiter Handle of the arbiter
 * @param addr A pointer to a s32 value.
 * @param type Type of action to be performed by the arbiter
 * @param value Number of threads to signal if using @ref ARBITRATION_SIGNAL, or the value used for comparison.
 * @param timeout_ns Optional timeout in nanoseconds when using TIMEOUT actions, ignored otherwise. If not needed, use \ref svcArbitrateAddressNoTimeout instead.
 * @note Usage of this syscall entails an implicit Data Memory Barrier (dmb).
 * @warning Please use \ref syncArbitrateAddressWithTimeout instead.
 */
static inline Result svcArbitrateAddress(Handle arbiter, u32 addr, ArbitrationType type, s32 value, s64 timeout_ns) {
	register const Handle _arbiter __asm__("r0") = arbiter;
	register u32 _addr __asm__("r1") = addr;
	register ArbitrationType _type __asm__("r2") = type;
	register s32 _value __asm__("r3") = value;
	register const u32 lo_ns __asm__("r4") = (u32)(((u64)timeout_ns) & ((u32)~0));
	register const u32 hi_ns __asm__("r5") = (u32)(((u64)timeout_ns) >> 32);

	register Result res __asm__("r0");

	__asm__ volatile ("svc\t0x22" : "=r"(res), "+r"(_addr), "+r"(_type), "+r"(_value) : "r"(_arbiter), "r"(lo_ns), "r"(hi_ns) : "r12", "memory");

	return res;
}

/**
 * @brief Sends a synchronized request to a session handle.
 * @param session Handle of the session.
 */
static inline Result svcSendSyncRequest(Handle session) {
	register const Handle _handle __asm__("r0") = session;

	register Result res __asm__("r0");

	__asm__ volatile ("svc\t0x32" : "=r"(res) : "r"(_handle) : "r1", "r2", "r3", "r12", "memory");

	return res;
}

/**
 * @brief Accepts a session.
 * @param[out] session Pointer to output the created session handle to.
 * @param port Handle of the port to accept a session from.
 */
static inline Result svcAcceptSession(Handle* session, Handle port) {
	register const Handle _port __asm__("r1") = port;

	register Result res __asm__("r0");
	register Handle out_handle __asm__("r1");

	__asm__ volatile ("svc\t0x4A" : "=r"(res), "=r"(out_handle) : "r"(_port) : "r2", "r3", "r12");

	*session = out_handle;

	return res;
}

/**
 * @brief Replies to and receives a new request.
 * @param index Pointer to the index of the request.
 * @param handles Session handles to receive requests from.
 * @param handleCount Number of handles.
 * @param replyTarget Handle of the session to reply to.
 */
static inline Result svcReplyAndReceive(s32* index, const Handle* handles, s32 handleCount, Handle replyTarget) {
	register const Handle* _handles __asm__("r1") = handles;
	register s32 _handleCount __asm__("r2") = handleCount;
	register Handle _replyTarget __asm__("r3") = replyTarget;

	register s32 _out_index __asm__("r1");
	register Result res __asm__("r0");

	__asm__ volatile ("svc\t0x4F" : "=r"(res), "=r"(_out_index), "+r"(_handleCount), "+r"(_replyTarget) : "r"(_handles) : "r12", "memory");

	*index = _out_index;

	return res;
}

/**
 * @brief Closes a handle.
 * @param handle Handle to close.
 */
static inline Result svcCloseHandle(Handle handle) {
	register const Handle _handle __asm__("r0") = handle;

	register Result res __asm__("r0");

	__asm__ volatile ("svc\t0x23" : "=r"(res) : "r"(_handle) : "r1", "r2", "r3", "r12");

	return res;
}

/**
 * @brief Breaks execution.
 * @param breakReason Reason for breaking.
 */
static inline void svcBreak(UserBreakType breakReason) {
	register UserBreakType _breakReason __asm__("r0") = breakReason;

	__asm__ volatile ("svc\t0x3C" : "+r"(_breakReason) : : "r1", "r2", "r3", "r12");
}

/// Stop point, does nothing if the process is not attached (as opposed to 'bkpt' instructions)
#define SVC_STOP_POINT __asm__ volatile("svc 0xFF");
