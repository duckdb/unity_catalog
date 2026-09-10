#pragma once

#include <utility>

#include "duckdb/common/mutex.hpp"

namespace duckdb {

// MutexProtected<T> fuses a mutex with the data it guards, so the data is reachable ONLY while the
// lock is held. There is no way to name the value without going through with_locked() — unlocked
// access is unrepresentable, not merely discouraged. (Pattern from SerenityOS, Andreas Kling.)
//
// Because access is gated by construction, a MutexProtected member is safe to expose publicly.
//
// with_locked(fn) acquires the lock, invokes fn(value), and releases on return, forwarding fn's
// result. Keep callbacks short and DO NOT block (network / file I/O) inside them — the lock is held
// for the callback's whole duration. To use a value past the critical section, copy it out:
//
//     auto snapshot = mp.with_locked([](const T &v) { return v; });
template <class T>
class MutexProtected {
public:
	template <class Fn>
	auto with_locked(Fn &&fn) -> decltype(fn(std::declval<T &>())) {
		lock_guard<mutex> l(mtx);
		return fn(value);
	}

	template <class Fn>
	auto with_locked(Fn &&fn) const -> decltype(fn(std::declval<const T &>())) {
		lock_guard<mutex> l(mtx);
		return fn(value);
	}

private:
	mutable mutex mtx;
	T value;
};

} // namespace duckdb
