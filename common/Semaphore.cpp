// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "common/Threading.h"
#include "common/Assertions.h"
#include "common/HostSys.h"

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#endif

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include <emscripten/threading.h>

#include <cmath>
#endif

#include <limits>

#ifdef __EMSCRIPTEN__
// Threads whose idle waits suspend instead of blocking (see Threading::SetWaitsSuspend()).
static thread_local bool s_waits_suspend = false;

// Set while the calling thread is suspended in KernelSemaphore::WaitSuspending(), whose event loop
// may then run tasks that call into wasm on this thread (see Threading::InSuspendingSemaphoreWait()).
static thread_local bool s_in_suspending_wait = false;

bool Threading::InSuspendingSemaphoreWait()
{
	return s_in_suspending_wait;
}

void Threading::SetWaitsSuspend(bool enabled)
{
	// Only a module linked with JSPI (or Asyncify) can suspend; anywhere else keep blocking.
	s_waits_suspend = enabled && emscripten_has_asyncify() != 0;
}

static void WaitForWorkerSema(Threading::KernelSemaphore& sema)
{
	if (s_waits_suspend)
		sema.WaitSuspending();
	else
		sema.Wait();
}
#endif

// --------------------------------------------------------------------------------------
//  Semaphore Implementations
// --------------------------------------------------------------------------------------

bool Threading::WorkSema::CheckForWork()
{
	s32 value = m_state.load(std::memory_order_relaxed);
	pxAssert(!IsDead(value));

	// we want to switch to the running state, but preserve the waiting empty bit for RUNNING_N -> RUNNING_0
	// otherwise, we clear the waiting flag (since we're notifying the waiter that we're empty below)
	while (!m_state.compare_exchange_weak(value,
		IsReadyForSleep(value) ? STATE_RUNNING_0 : (value & STATE_FLAG_WAITING_EMPTY),
		std::memory_order_acq_rel, std::memory_order_relaxed))
	{
	}

	// if we're not empty, we have work to do
	if (!IsReadyForSleep(value))
		return true;

	// this means we're empty, so notify any waiters
	if (value & STATE_FLAG_WAITING_EMPTY)
		m_empty_sema.Post();

	// no work to do
	return false;
}

void Threading::WorkSema::WaitForWork()
{
	// State change:
	// SLEEPING, SPINNING: This is the worker thread and it's clearly not asleep or spinning, so these states should be impossible
	// RUNNING_0: Change state to SLEEPING, wake up thread if WAITING_EMPTY
	// RUNNING_N: Change state to RUNNING_0 (and preserve WAITING_EMPTY flag)
	s32 value = m_state.load(std::memory_order_relaxed);
	pxAssert(!IsDead(value));
	while (!m_state.compare_exchange_weak(value, NextStateWaitForWork(value), std::memory_order_acq_rel, std::memory_order_relaxed))
		;
	if (IsReadyForSleep(value))
	{
		if (value & STATE_FLAG_WAITING_EMPTY)
			m_empty_sema.Post();
#ifdef __EMSCRIPTEN__
		WaitForWorkerSema(m_sema);
#else
		m_sema.Wait();
#endif
		// Acknowledge any additional work added between wake up request and getting here
		m_state.fetch_and(STATE_FLAG_WAITING_EMPTY, std::memory_order_acquire);
	}
}

void Threading::WorkSema::WaitForWorkWithSpin()
{
	s32 value = m_state.load(std::memory_order_relaxed);
	pxAssert(!IsDead(value));
	while (IsReadyForSleep(value))
	{
		if (m_state.compare_exchange_weak(value, STATE_SPINNING, std::memory_order_release, std::memory_order_relaxed))
		{
			if (value & STATE_FLAG_WAITING_EMPTY)
				m_empty_sema.Post();
			value = STATE_SPINNING;
			break;
		}
	}
	u32 waited = 0;
	while (value < 0)
	{
		if (waited > SPIN_TIME_NS)
		{
			if (!m_state.compare_exchange_weak(value, STATE_SLEEPING, std::memory_order_relaxed))
				continue;
#ifdef __EMSCRIPTEN__
			WaitForWorkerSema(m_sema);
#else
			m_sema.Wait();
#endif
			break;
		}
		waited += ShortSpin();
		value = m_state.load(std::memory_order_relaxed);
	}
	// Clear back to STATE_RUNNING_0 (but preserve waiting empty flag)
	m_state.fetch_and(STATE_FLAG_WAITING_EMPTY, std::memory_order_acquire);
}

bool Threading::WorkSema::WaitForEmpty()
{
	s32 value = m_state.load(std::memory_order_acquire);
	while (true)
	{
		if (value < 0)
			return !IsDead(value); // STATE_SLEEPING or STATE_SPINNING, queue is empty!
		// Note: We technically only need memory_order_acquire on *failure* (because that's when we could leave without sleeping), but libstdc++ still asserts on failure < success
		if (m_state.compare_exchange_weak(value, value | STATE_FLAG_WAITING_EMPTY, std::memory_order_acquire))
			break;
	}
	pxAssertMsg(!(value & STATE_FLAG_WAITING_EMPTY), "Multiple threads attempted to wait for empty (not currently supported)");
	m_empty_sema.Wait();
	return !IsDead(m_state.load(std::memory_order_relaxed));
}

bool Threading::WorkSema::WaitForEmptyWithSpin()
{
	s32 value = m_state.load(std::memory_order_acquire);
	u32 waited = 0;
	while (true)
	{
		if (value < 0)
			return !IsDead(value); // STATE_SLEEPING or STATE_SPINNING, queue is empty!
		if (waited > SPIN_TIME_NS && m_state.compare_exchange_weak(value, value | STATE_FLAG_WAITING_EMPTY, std::memory_order_acquire))
			break;
		waited += ShortSpin();
		value = m_state.load(std::memory_order_acquire);
	}
	pxAssertMsg(!(value & STATE_FLAG_WAITING_EMPTY), "Multiple threads attempted to wait for empty (not currently supported)");
	m_empty_sema.Wait();
	return !IsDead(m_state.load(std::memory_order_relaxed));
}

void Threading::WorkSema::Kill()
{
	s32 value = m_state.exchange(std::numeric_limits<s32>::min(), std::memory_order_release);
	if (value & STATE_FLAG_WAITING_EMPTY)
		m_empty_sema.Post();
}

void Threading::WorkSema::Reset()
{
	m_state = STATE_RUNNING_0;
}

#if defined(__EMSCRIPTEN__)

// Emscripten's sem_t can only block. This is musl's sem_t algorithm (sem_post.c, sem_timedwait.c:
// count with a waiters flag in bit 31, a waiter count, a short spin before sleeping, wake-ups only
// when someone waits) on words of our own, waiting through the same futex calls when blocking and
// through Atomics.waitAsync under JSPI when suspending. Atomics.notify wakes both kinds of waiter.
// (Not emscripten_atomic_wait_suspending(): in 6.0.12 it never frees the promise id it allocates,
// one per wait.) Emscripten refreshes HEAP32 after memory growth (it emits growMemViews() before
// each use); >>> keeps addresses past 2 GB positive.
EM_ASYNC_JS(void, pcsx2_futex_wait_suspending, (const void* addr, u32 value), {
	const wait = Atomics.waitAsync(HEAP32, addr >>> 2, value | 0);
	if (wait.async)
		await wait.value;
});

static constexpr s32 SEMA_WAITERS_FLAG = std::numeric_limits<s32>::min();
static constexpr s32 SEMA_VALUE_MASK = std::numeric_limits<s32>::max();

Threading::KernelSemaphore::KernelSemaphore() = default;

Threading::KernelSemaphore::~KernelSemaphore() = default;

void Threading::KernelSemaphore::Post()
{
	s32 value = m_value.load();
	s32 waiters, new_value;
	do
	{
		waiters = m_waiters.load();
		new_value = value + 1;
		if (waiters <= 1)
			new_value &= ~SEMA_WAITERS_FLAG;
	} while (!m_value.compare_exchange_weak(value, new_value));

	if (value < 0 || waiters != 0)
		emscripten_futex_wake(&m_value, (waiters > 1) ? 1 : std::numeric_limits<int>::max());
}

template <bool suspend>
void Threading::KernelSemaphore::WaitImpl()
{
	if (TryWait())
		return;

	for (int spins = 100; spins > 0 && !(m_value.load() & SEMA_VALUE_MASK) && m_waiters.load() == 0; spins--)
		;

	while (!TryWait())
	{
		m_waiters.fetch_add(1);
		s32 expected = 0;
		m_value.compare_exchange_strong(expected, SEMA_WAITERS_FLAG);
		if constexpr (suspend)
			pcsx2_futex_wait_suspending(&m_value, static_cast<u32>(SEMA_WAITERS_FLAG));
		else
			emscripten_futex_wait(&m_value, static_cast<u32>(SEMA_WAITERS_FLAG), INFINITY);
		m_waiters.fetch_sub(1);
	}
}

void Threading::KernelSemaphore::Wait()
{
	WaitImpl<false>();
}

void Threading::KernelSemaphore::WaitSuspending()
{
	// Needs proper testing outside Chrome: the spike measured 0.045-0.06 ms per wake-up in Chrome.
	pxAssertMsg(!s_in_suspending_wait, "Suspending wait entered while the thread is suspended");
	s_in_suspending_wait = true;
	WaitImpl<true>();
	s_in_suspending_wait = false;
}

bool Threading::KernelSemaphore::TryWait()
{
	s32 value = m_value.load();
	while (value & SEMA_VALUE_MASK)
	{
		if (m_value.compare_exchange_weak(value, value - 1))
			return true;
	}
	return false;
}

#elif !defined(__APPLE__) // macOS implementations are in DarwinThreads

Threading::KernelSemaphore::KernelSemaphore()
{
#ifdef _WIN32
	m_sema = CreateSemaphore(nullptr, 0, LONG_MAX, nullptr);
#else
	sem_init(&m_sema, false, 0);
#endif
}

Threading::KernelSemaphore::~KernelSemaphore()
{
#ifdef _WIN32
	CloseHandle(m_sema);
#else
	sem_destroy(&m_sema);
#endif
}

void Threading::KernelSemaphore::Post()
{
#ifdef _WIN32
	ReleaseSemaphore(m_sema, 1, nullptr);
#else
	sem_post(&m_sema);
#endif
}

void Threading::KernelSemaphore::Wait()
{
#ifdef _WIN32
	WaitForSingleObject(m_sema, INFINITE);
#else
	sem_wait(&m_sema);
#endif
}

bool Threading::KernelSemaphore::TryWait()
{
#ifdef _WIN32
	return WaitForSingleObject(m_sema, 0) == WAIT_OBJECT_0;
#else
	return sem_trywait(&m_sema) == 0;
#endif
}

#endif
