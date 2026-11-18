// ----------------------------------------------------------------------------
//      threads.cxx
//
// Copyright (C) 2014
//              Stelios Bounanos, M0GLD
//              David Freese, W1HKJ
//
// This file is part of fldigi.
//
// fldigi is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 3 of the License, or
// (at your option) any later version.
//
// fldigi is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
// ----------------------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <chrono>
#include <map>
#include <string>

#include "threads.h"
#include "util.h"
#include "support.h"
#include "debug.h"

/// This ensures that a mutex is always unlocked when leaving a function or block.

extern pthread_mutex_t mutex_replystr;
extern pthread_mutex_t command_mutex;
extern pthread_mutex_t mutex_serial;
extern pthread_mutex_t debug_mutex;
extern pthread_mutex_t mutex_rcv_socket;
extern pthread_mutex_t mutex_trace;

// [subspace] Lock wait limit.
// LOCK_SLOW_MARK_MS: the wait that is logged as slow (as before: 1 s).
// LOCK_WAIT_LIMIT_MS: the wait after which the lock is reported as not free,
// with the name of its holder, and -- on a thread that can answer "busy"
// (lock_wait_give_up) -- the request is refused.
// CHOSEN: 3000 ms. Above the slow mark and above the slowest answer measured
// at a working station (752 ms, W5TTA 2026-11-17), so a busy radio does not
// reach it; short enough that the program that asked is told within a few
// seconds instead of never.
#define LOCK_SLOW_MARK_MS  1000
#define LOCK_WAIT_LIMIT_MS 3000

thread_local void (*lock_wait_give_up)(const char *what) = 0;

// guard_locks this thread holds now
static thread_local int guard_locks_held = 0;

// Who holds each mutex taken with guard_lock, and since when: read by a
// thread whose wait reached the limit. holders_mutex is taken last and
// nothing is called while it is held. The map is never destroyed (threads
// still lock while the program exits).
struct lock_holder {
	bool held;
	std::string how;
	std::chrono::steady_clock::time_point since;
};
static pthread_mutex_t holders_mutex = PTHREAD_MUTEX_INITIALIZER;
static std::map<pthread_mutex_t*, lock_holder> & holders()
{
	static std::map<pthread_mutex_t*, lock_holder> *h = new std::map<pthread_mutex_t*, lock_holder>;
	return *h;
}

static void holder_set(pthread_mutex_t *m, const std::string &how)
{
	pthread_mutex_lock(&holders_mutex);
	lock_holder &h = holders()[m];
	h.held = true;
	h.how = how;
	h.since = std::chrono::steady_clock::now();
	pthread_mutex_unlock(&holders_mutex);
}

static void holder_clear(pthread_mutex_t *m)
{
	pthread_mutex_lock(&holders_mutex);
	holders()[m].held = false;
	pthread_mutex_unlock(&holders_mutex);
}

// "held 5200 ms by 'serviceXCVR'"
static std::string holder_text(pthread_mutex_t *m)
{
	std::string text = "holder not recorded";   // taken with pthread_mutex_lock, not guard_lock
	pthread_mutex_lock(&holders_mutex);
	std::map<pthread_mutex_t*, lock_holder>::iterator it = holders().find(m);
	if (it != holders().end() && it->second.held) {
		long ms = (long)std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - it->second.since).count();
		text.assign("held ").append(std::to_string(ms)).append(" ms by '")
			.append(it->second.how.empty() ? "(no label)" : it->second.how).append("'");
	}
	pthread_mutex_unlock(&holders_mutex);
	return text;
}

#if defined(__GLIBC__)
#  if __GLIBC_PREREQ(2, 30)
#    define HAVE_MUTEX_CLOCKLOCK 1
#  endif
#endif

// true: the mutex was obtained within ms. Timed on a clock that does not
// jump when the PC clock is set (stations run clock-sync programs): a jump
// forwards must not end the wait early.
static bool lock_within(pthread_mutex_t *m, long ms)
{
#ifdef HAVE_MUTEX_CLOCKLOCK
	// wakes the moment the mutex is released
	struct timespec deadline;
	clock_gettime(CLOCK_MONOTONIC, &deadline);
	deadline.tv_sec += ms / 1000;
	deadline.tv_nsec += (ms % 1000) * 1000000L;
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_sec++;
		deadline.tv_nsec -= 1000000000L;
	}
	return pthread_mutex_clocklock(m, CLOCK_MONOTONIC, &deadline) == 0;
#else
	// macOS, Windows, older C libraries: no timed lock on a steady clock
	std::chrono::steady_clock::time_point const deadline =
		std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
	for (;;) {
		if (pthread_mutex_trylock(m) == 0) return true;
		if (std::chrono::steady_clock::now() >= deadline) return false;
		MilliSleep(1);
	}
#endif
}

guard_lock::guard_lock(pthread_mutex_t* m, std::string h, long tout) : mutex(m), held(false) {

	how.clear();
	how = h;
	time_out = tout;
	start_time = zmsec();
// Wait up to the slow mark, waking the moment the mutex is released (the
// previous trylock + 50 ms sleep loop added up to 50 ms to every contended
// lock), log it, and wait on up to the limit. Past the limit this thread
// NEVER goes on without the lock: that let two threads use the serial port
// at once (mixed replies, and the destructor then unlocked a mutex this
// thread did not own). It either refuses the request (lock_wait_give_up) or
// keeps waiting.
	bool locked = lock_within(mutex, LOCK_SLOW_MARK_MS);
	if (!locked) {
		std::string szlock;;
		szlock.assign("lock SLOW (>1 s) ").append(name(mutex));
		if (!h.empty())
			szlock.append(", ").append(h);
		failure_trace(1, szlock.c_str());
		locked = lock_within(mutex, LOCK_WAIT_LIMIT_MS - LOCK_SLOW_MARK_MS);
	}
	if (!locked) {
		std::string what = name(mutex);
		what.append(" not free after ").append(std::to_string(LOCK_WAIT_LIMIT_MS)).append(" ms, ")
			.append(holder_text(mutex)).append("; wanted by '").append(h).append("'");
		bool const refuse = lock_wait_give_up && guard_locks_held == 0;
		LOG_ERROR("%s: %s", what.c_str(), refuse ? "request refused" : "still waiting");
		if (refuse)
			lock_wait_give_up(what.c_str());   // does not return
		pthread_mutex_lock(mutex);
		LOG_ERROR("%s obtained after %ld ms, wanted by '%s'", name(mutex), (long)(zmsec() - start_time), h.c_str());
	}

	held = true;
	guard_locks_held++;
	holder_set(mutex, how);

	std::string szlock = name(mutex);
	szlock.append(" try lock ");
	if (!h.empty())
		szlock.append(", ").append(how);
	lock_trace(1, szlock.c_str());
}

guard_lock::~guard_lock(void) {
	unlock();
}

void guard_lock::unlock(void) {

	if (!held) return;

	char szlock[200];
	long now = zmsec();
	snprintf(szlock, sizeof(szlock), "%s locked for %lu msec", name(mutex), (now - start_time));
	lock_trace(1, szlock);

	if (now - start_time > time_out) {
		snprintf(szlock, sizeof(szlock), "%s [ %s ] LOCK TIME: %lu", name(mutex), how.c_str(), (now - start_time));
		failure_trace(1, szlock);
	}

	held = false;
	guard_locks_held--;
	holder_clear(mutex);
	pthread_mutex_unlock(mutex);
}

const char * guard_lock::name(pthread_mutex_t *m) {
	if (m == &mutex_replystr) return "mutex_replystr";
	if (m == &command_mutex) return "command_mutex";
	if (m == &mutex_replystr) return "mutex_replystr";
	if (m == &mutex_serial) return "mutex_serial";
	if (m == &debug_mutex) return "debug_mutex";
	if (m == &mutex_rcv_socket) return "mutex_rcv_socket";
	if (m == &mutex_srvc_reqs) return "mutex_service_requests";
	if (m == &mutex_trace) return "mutex_trace";
	return "";
}

#ifndef __WIN_32_

int nano_sleep(const struct timespec *req, struct timespec *rem)
{
	return nanosleep(req, rem);
}

#else

int nano_sleep(const struct timespec *req, struct timespec *rem)
{
	if (unlikely(req->tv_nsec < 0 || req->tv_nsec < 0L || req->tv_nsec > 999999999L)) {
		errno = EINVAL;
		return -1;
	}
	Sleep(req->tv_sec * 1000 + req->tv_nsec / 1000000L);
	if (unlikely(rem)) {
		rem->tv_sec = 0;
		rem->tv_nsec = 0L;
	}
	return 0;
}

#endif
