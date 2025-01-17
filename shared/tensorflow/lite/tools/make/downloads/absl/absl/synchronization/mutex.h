// Copyright 2017 The Abseil Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// -----------------------------------------------------------------------------
// mutex.h
// -----------------------------------------------------------------------------
//
// This header file defines a `Mutex` -- a mutually exclusive lock -- and the
// most common type of synchronization primitive for facilitating locks on
// shared resources. A mutex is used to prevent multiple threads from accessing
// and/or writing to a shared resource concurrently.
//
// Unlike a `std::mutex`, the Abseil `Mutex` provides the following additional
// features:
//   * Conditional predicates intrinsic to the `Mutex` object
//   * Shared/reader locks, in addition to standard exclusive/writer locks
//   * Deadlock detection and debug support.
//
// The following helper classes are also defined within this file:
//
//  MutexLock - An RAII wrapper to acquire and release a `Mutex` for exclusive/
//              write access within the current scope.
//  ReaderMutexLock
//            - An RAII wrapper to acquire and release a `Mutex` for shared/read
//              access within the current scope.
//
//  WriterMutexLock
//            - Alias for `MutexLock` above, designed for use in distinguishing
//              reader and writer locks within code.
//
// In addition to simple mutex locks, this file also defines ways to perform
// locking under certain conditions.
//
//  Condition   - (Preferred) Used to wait for a particular predicate that
//                depends on state protected by the `Mutex` to become true.
//  CondVar     - A lower-level variant of `Condition` that relies on
//                application code to explicitly signal the `CondVar` when
//                a condition has been met.
//
// See below for more information on using `Condition` or `CondVar`.
//
// Mutexes and mutex behavior can be quite complicated. The information within
// this header file is limited, as a result. Please consult the Mutex guide for
// more complete information and examples.

#ifndef ABSL_SYNCHRONIZATION_MUTEX_H_
#define ABSL_SYNCHRONIZATION_MUTEX_H_

#include <atomic>
#include <cstdint>
#include <string>

#include "absl/base/internal/identity.h"
#include "absl/base/internal/low_level_alloc.h"
#include "absl/base/internal/thread_identity.h"
#include "absl/base/internal/tsan_mutex_interface.h"
#include "absl/base/port.h"
#include "absl/base/thread_annotations.h"
#include "absl/synchronization/internal/kernel_timeout.h"
#include "absl/synchronization/internal/per_thread_sem.h"
#include "absl/time/time.h"

// Decide if we should use the non-production implementation because
// the production implementation hasn't been fully ported yet.
#ifdef ABSL_INTERNAL_USE_NONPROD_MUTEX
#error ABSL_INTERNAL_USE_NONPROD_MUTEX cannot be directly set
#elif defined(ABSL_LOW_LEVEL_ALLOC_MISSING)
#define ABSL_INTERNAL_USE_NONPROD_MUTEX 1
#include "absl/synchronization/internal/mutex_nonprod.inc"
#endif

namespace absl {

class Condition;
struct SynchWaitParams;

// -----------------------------------------------------------------------------
// Mutex
// -----------------------------------------------------------------------------
//
// A `Mutex` is a non-reentrant (aka non-recursive) Mutually Exclusive lock
// on some resource, typically a variable or data structure with associated
// invariants. Proper usage of mutexes prevents concurrent access by different
// threads to the same resource.
//
// A `Mutex` has two basic operations: `Mutex::Lock()` and `Mutex::Unlock()`.
// The `Lock()` operation *acquires* a `Mutex` (in a state known as an
// *exclusive* -- or write -- lock), while the `Unlock()` operation *releases* a
// Mutex. During the span of time between the Lock() and Unlock() operations,
// a mutex is said to be *held*. By design all mutexes support exclusive/write
// locks, as this is the most common way to use a mutex.
//
// The `Mutex` state machine for basic lock/unlock operations is quite simple:
//
// |                | Lock()     | Unlock() |
// |----------------+------------+----------|
// | Free           | Exclusive  | invalid  |
// | Exclusive      | blocks     | Free     |
//
// Attempts to `Unlock()` must originate from the thread that performed the
// corresponding `Lock()` operation.
//
// An "invalid" operation is disallowed by the API. The `Mutex` implementation
// is allowed to do anything on an invalid call, including but not limited to
// crashing with a useful error message, silently succeeding, or corrupting
// data structures. In debug mode, the implementation attempts to crash with a
// useful error message.
//
// `Mutex` is not guaranteed to be "fair" in prioritizing waiting threads; it
// is, however, approximately fair over long periods, and starvation-free for
// threads at the same priority.
//
// The lock/unlock primitives are now annotated with lock annotations
// defined in (base/thread_annotations.h). When writing multi-threaded code,
// you should use lock annotations whenever possible to document your lock
// synchronization policy. Besides acting as documentation, these annotations
// also help compilers or static analysis tools to identify and warn about
// issues that could potentially result in race conditions and deadlocks.
//
// For more information about the lock annotations, please see
// [Thread Safety
// Analysis](http://clang.llvm.org/docs/ThreadSafetyAnalysis.html) in the Clang
// documentation.
//
// See also `MutexLock`, below, for scoped `Mutex` acquisition.

class LOCKABLE Mutex {
 public:
  Mutex();
  ~Mutex();

  // Mutex::Lock()
  //
  // Blocks the calling thread, if necessary, until this `Mutex` is free, and
  // then acquires it exclusively. (This lock is also known as a "write lock.")
  void Lock() EXCLUSIVE_LOCK_FUNCTION();

  // Mutex::Unlock()
  //
  // Releases this `Mutex` and returns it from the exclusive/write state to the
  // free state. Caller must hold the `Mutex` exclusively.
  void Unlock() UNLOCK_FUNCTION();

  // Mutex::TryLock()
  //
  // If the mutex can be acquired without blocking, does so exclusively and
  // returns `true`. Otherwise, returns `false`. Returns `true` with high
  // probability if the `Mutex` was free.
  bool TryLock() EXCLUSIVE_TRYLOCK_FUNCTION(true);

  // Mutex::AssertHeld()
  //
  // Return immediately if this thread holds the `Mutex` exclusively (in write
  // mode). Otherwise, may report an error (typically by crashing with a
  // diagnostic), or may return immediately.
  void AssertHeld() const ASSERT_EXCLUSIVE_LOCK();

  // ---------------------------------------------------------------------------
  // Reader-Writer Locking
  // ---------------------------------------------------------------------------

  // A Mutex can also be used as a starvation-free reader-writer lock.
  // Neither read-locks nor write-locks are reentrant/recursive to avoid
  // potential client programming errors.
  //
  // The Mutex API provides `Writer*()` aliases for the existing `Lock()`,
  // `Unlock()` and `TryLock()` methods for use within applications mixing
  // reader/writer locks. Using `Reader*()` and `Writer*()` operations in this
  // manner can make locking behavior clearer when mixing read and write modes.
  //
  // Introducing reader locks necessarily complicates the `Mutex` state
  // machine somewhat. The table below illustrates the allowed state transitions
  // of a mutex in such cases. Note that ReaderLock() may block even if the lock
  // is held in shared mode; this occurs when another thread is blocked on a
  // call to WriterLock().
  //
  // ---------------------------------------------------------------------------
  //     Operation: WriterLock() Unlock()  ReaderLock()           ReaderUnlock()
  // ---------------------------------------------------------------------------
  // State
  // ---------------------------------------------------------------------------
  // Free           Exclusive    invalid   Shared(1)              invalid
  // Shared(1)      blocks       invalid   Shared(2) or blocks    Free
  // Shared(n) n>1  blocks       invalid   Shared(n+1) or blocks  Shared(n-1)
  // Exclusive      blocks       Free      blocks                 invalid
  // ---------------------------------------------------------------------------
  //
  // In comments below, "shared" refers to a state of Shared(n) for any n > 0.

  // Mutex::ReaderLock()
  //
  // Blocks the calling thread, if necessary, until this `Mutex` is either free,
  // or in shared mode, and then acquires a share of it. Note that
  // `ReaderLock()` will block if some other thread has an exclusive/writer lock
  // on the mutex.

  void ReaderLock() SHARED_LOCK_FUNCTION();

  // Mutex::ReaderUnlock()
  //
  // Releases a read share of this `Mutex`. `ReaderUnlock` may return a mutex to
  // the free state if this thread holds the last reader lock on the mutex. Note
  // that you cannot call `ReaderUnlock()` on a mutex held in write mode.
  void ReaderUnlock() UNLOCK_FUNCTION();

  // Mutex::ReaderTryLock()
  //
  // If the mutex can be acquired without blocking, acquires this mutex for
  // shared access and returns `true`. Otherwise, returns `false`. Returns
  // `true` with high probability if the `Mutex` was free or shared.
  bool ReaderTryLock() SHARED_TRYLOCK_FUNCTION(true);

  // Mutex::AssertReaderHeld()
  //
  // Returns immediately if this thread holds the `Mutex` in at least shared
  // mode (read mode). Otherwise, may report an error (typically by
  // crashing with a diagnostic), or may return immediately.
  void AssertReaderHeld() const ASSERT_SHARED_LOCK();

  // Mutex::WriterLock()
  // Mutex::WriterUnlock()
  // Mutex::WriterTryLock()
  //
  // Aliases for `Mutex::Lock()`, `Mutex::Unlock()`, and `Mutex::TryLock()`.
  //
  // These methods may be used (along with the complementary `Reader*()`
  // methods) to distingish simple exclusive `Mutex` usage (`Lock()`,
  // etc.) from reader/writer lock usage.
  void WriterLock() EXCLUSIVE_LOCK_FUNCTION() { this->Lock(); }

  void WriterUnlock() UNLOCK_FUNCTION() { this->Unlock(); }

  bool WriterTryLock() EXCLUSIVE_TRYLOCK_FUNCTION(true) {
    return this->TryLock();
  }

  // ---------------------------------------------------------------------------
  // Conditional Critical Regions
  // ---------------------------------------------------------------------------

  // Conditional usage of a `Mutex` can occur using two distinct paradigms:
  //
  //   * Use of `Mutex` member functions with `Condition` objects.
  //   * Use of the separate `CondVar` abstraction.
  //
  // In general, prefer use of `Condition` and the `Mutex` member functions
  // listed below over `CondVar`. When there are multiple threads waiting on
  // distinctly different conditions, however, a battery of `CondVar`s may be
  // more efficient. This section discusses use of `Condition` objects.
  //
  // `Mutex` contains member functions for performing lock operations only under
  // certain conditions, of class `Condition`. For correctness, the `Condition`
  // must return a boolean that is a pure function, only of state protected by
  // the `Mutex`. The condition must be invariant w.r.t. environmental state
  // such as thread, cpu id, or time, and must be `noexcept`. The condition will
  // always be invoked with the mutex held in at least read mode, so you should
  // not block it for long periods or sleep it on a timer.
  //
  // Since a condition must not depend directly on the current time, use
  // `*WithTimeout()` member function variants to make your condition
  // effectively true after a given duration, or `*WithDeadline()` variants to
  // make your condition effectively true after a given time.
  //
  // The condition function should have no side-effects aside from debug
  // logging; as a special exception, the function may acquire other mutexes
  // provided it releases all those that it acquires.  (This exception was
  // required to allow logging.)

  // Mutex::Await()
  //
  // Unlocks this `Mutex` and blocks until simultaneously both `cond` is `true`
  // and this `Mutex` can be reacquired, then reacquires this `Mutex` in the
  // same mode in which it was previously held. If the condition is initially
  // `true`, `Await()` *may* skip the release/re-acquire step.
  //
  // `Await()` requires that this thread holds this `Mutex` in some mode.
  void Await(const Condition& cond);

  // Mutex::LockWhen()
  // Mutex::ReaderLockWhen()
  // Mutex::WriterLockWhen()
  //
  // Blocks until simultaneously both `cond` is `true` and this `Mutex` can
  // be acquired, then atomically acquires this `Mutex`. `LockWhen()` is
  // logically equivalent to `*Lock(); Await();` though they may have different
  // performance characteristics.
  void LockWhen(const Condition& cond) EXCLUSIVE_LOCK_FUNCTION();

  void ReaderLockWhen(const Condition& cond) SHARED_LOCK_FUNCTION();

  void WriterLockWhen(const Condition& cond) EXCLUSIVE_LOCK_FUNCTION() {
    this->LockWhen(cond);
  }

  // ---------------------------------------------------------------------------
  // Mutex Variants with Timeouts/Deadlines
  // ---------------------------------------------------------------------------

  // Mutex::AwaitWithTimeout()
  // Mutex::AwaitWithDeadline()
  //
  // If `cond` is initially true, do nothing, or act as though `cond` is
  // initially false.
  //
  // If `cond` is initially false, unlock this `Mutex` and block until
  // simultaneously:
  //   - either `cond` is true or the {timeout has expired, deadline has passed}
  //     and
  //   - this `Mutex` can be reacquired,
  // then reacquire this `Mutex` in the same mode in which it was previously
  // held, returning `true` iff `cond` is `true` on return.
  //
  // Deadlines in the past are equivalent to an immediate deadline.
  // Negative timeouts are equivalent to a zero timeout.
  //
  // This method requires that this thread holds this `Mutex` in some mode.
  bool AwaitWithTimeout(const Condition& cond, absl::Duration timeout);

  bool AwaitWithDeadline(const Condition& cond, absl::Time deadline);

  // Mutex::LockWhenWithTimeout()
  // Mutex::ReaderLockWhenWithTimeout()
  // Mutex::WriterLockWhenWithTimeout()
  //
  // Blocks until simultaneously both:
  //   - either `cond` is `true` or the timeout has expired, and
  //   - this `Mutex` can be acquired,
  // then atomically acquires this `Mutex`, returning `true` iff `cond` is
  // `true` on return.
  //
  // Negative timeouts are equivalent to a zero timeout.
  bool LockWhenWithTimeout(const Condition& cond, absl::Duration timeout)
      EXCLUSIVE_LOCK_FUNCTION();
  bool ReaderLockWhenWithTimeout(const Condition& cond, absl::Duration timeout)
      SHARED_LOCK_FUNCTION();
  bool WriterLockWhenWithTimeout(const Condition& cond, absl::Duration timeout)
      EXCLUSIVE_LOCK_FUNCTION() {
    return this->LockWhenWithTimeout(cond, timeout);
  }

  // Mutex::LockWhenWithDeadline()
  // Mutex::ReaderLockWhenWithDeadline()
  // Mutex::WriterLockWhenWithDeadline()
  //
  // Blocks until simultaneously both:
  //   - either `cond` is `true` or the deadline has been passed, and
  //   - this `Mutex` can be acquired,
  // then atomically acquires this Mutex, returning `true` iff `cond` is `true`
  // on return.
  //
  // Deadlines in the past are equivalent to an immediate deadline.
  bool LockWhenWithDeadline(const Condition& cond, absl::Time deadline)
      EXCLUSIVE_LOCK_FUNCTION();
  bool ReaderLockWhenWithDeadline(const Condition& cond, absl::Time deadline)
      SHARED_LOCK_FUNCTION();
  bool WriterLockWhenWithDeadline(const Condition& cond, absl::Time deadline)
      EXCLUSIVE_LOCK_FUNCTION() {
    return this->LockWhenWithDeadline(cond, deadline);
  }

  // ---------------------------------------------------------------------------
  // Debug Support: Invariant Checking, Deadlock Detection, Logging.
  // ---------------------------------------------------------------------------

  // Mutex::EnableInvariantDebugging()
  //
  // If `invariant`!=null and if invariant debugging has been enabled globally,
  // cause `(*invariant)(arg)` to be called at moments when the invariant for
  // this `Mutex` should hold (for example: just after acquire, just before
  // release).
  //
  // The routine `invariant` should have no side-effects since it is not
  // guaranteed how many times it will be called; it should check the invariant
  // and crash if it does not hold. Enabling global invariant debugging may
  // substantially reduce `Mutex` performance; it should be set only for
  // non-production runs.  Optimization options may also disable invariant
  // checks.
  void EnableInvariantDebugging(void (*invariant)(void*), void* arg);

  // Mutex::EnableDebugLog()
  //
  // Cause all subsequent uses of this `Mutex` to be logged via
  // `ABSL_RAW_LOG(INFO)`. Log entries are tagged with `name` if no previous
  // call to `EnableInvariantDebugging()` or `EnableDebugLog()` has been made.
  //
  // Note: This method substantially reduces `Mutex` performance.
  void EnableDebugLog(const char* name);

  // Deadlock detection

  // Mutex::ForgetDeadlockInfo()
  //
  // Forget any deadlock-detection information previously gathered
  // about this `Mutex`. Call this method in debug mode when the lock ordering
  // of a `Mutex` changes.
  void ForgetDeadlockInfo();

  // Mutex::AssertNotHeld()
  //
  // Return immediately if this thread does not hold this `Mutex` in any
  // mode; otherwise, may report an error (typically by crashing with a
  // diagnostic), or may return immediately.
  //
  // Currently this check is performed only if all of:
  //    - in debug mode
  //    - SetMutexDeadlockDetectionMode() has been set to kReport or kAbort
  //    - number of locks concurrently held by this thread is not large.
  // are true.
  void AssertNotHeld() const;

  // Special cases.

  // A `MuHow` is a constant that indicates how a lock should be acquired.
  // Internal implementation detail.  Clients should ignore.
  typedef const struct MuHowS* MuHow;

  // Mutex::InternalAttemptToUseMutexInFatalSignalHandler()
  //
  // Causes the `Mutex` implementation to prepare itself for re-entry caused by
  // future use of `Mutex` within a fatal signal handler. This method is
  // intended for use only for last-ditch attempts to log crash information.
  // It does not guarantee that attempts to use Mutexes within the handler will
  // not deadlock; it merely makes other faults less likely.
  //
  // WARNING:  This routine must be invoked from a signal handler, and the
  // signal handler must either loop forever or terminate the process.
  // Attempts to return from (or `longjmp` out of) the signal handler once this
  // call has been made may cause arbitrary program behaviour including
  // crashes and deadlocks.
  static void InternalAttemptToUseMutexInFatalSignalHandler();

 private:
#ifdef ABSL_INTERNAL_USE_NONPROD_MUTEX
  friend class CondVar;

  synchronization_internal::MutexImpl* impl() { return impl_.get(); }

  synchronization_internal::SynchronizationStorage<
      synchronization_internal::MutexImpl>
      impl_;
#else
  std::atomic<intptr_t> mu_;  // The Mutex state.

  // Post()/Wait() versus associated PerThreadSem; in class for required
  // friendship with PerThreadSem.
  static inline void IncrementSynchSem(Mutex* mu,
                                       base_internal::PerThreadSynch* w);
  static inline bool DecrementSynchSem(
      Mutex* mu, base_internal::PerThreadSynch* w,
      synchronization_internal::KernelTimeout t);

  // slow path acquire
  void LockSlowLoop(SynchWaitParams* waitp, int flags);
  // wrappers around LockSlowLoop()
  bool LockSlowWithDeadline(MuHow how, const Condition* cond,
                            synchronization_internal::KernelTimeout t,
                            int flags);
  void LockSlow(MuHow how, const Condition* cond,
                int flags) ABSL_ATTRIBUTE_COLD;
  // slow path release
  void UnlockSlow(SynchWaitParams* waitp) ABSL_ATTRIBUTE_COLD;
  // Common code between Await() and AwaitWithTimeout/Deadline()
  bool AwaitCommon(const Condition& cond,
                   synchronization_internal::KernelTimeout t);
  // Attempt to remove thread s from queue.
  void TryRemove(base_internal::PerThreadSynch* s);
  // Block a thread on mutex.
  void Block(base_internal::PerThreadSynch* s);
  // Wake a thread; return successor.
  base_internal::PerThreadSynch* Wakeup(base_internal::PerThreadSynch* w);

  friend class CondVar;   // for access to Trans()/Fer().
  void Trans(MuHow how);  // used for CondVar->Mutex transfer
  void Fer(
      base_internal::PerThreadSynch* w);  // used for CondVar->Mutex transfer
#endif

  // Catch the error of writing Mutex when intending MutexLock.
  Mutex(const volatile Mutex* /*ignored*/) {}  // NOLINT(runtime/explicit)

  Mutex(const Mutex&) = delete;
  Mutex& operator=(const Mutex&) = delete;
};

// -----------------------------------------------------------------------------
// Mutex RAII Wrappers
// -----------------------------------------------------------------------------

// MutexLock
//
// `MutexLock` is a helper class, which acquires and releases a `Mutex` via
// RAII.
//
// Example:
//
// Class Foo {
//
//   Foo::Bar* Baz() {
//     MutexLock l(&lock_);
//     ...
//     return bar;
//   }
//
// private:
//   Mutex lock_;
// };
class SCOPED_LOCKABLE MutexLock {
 public:
  explicit MutexLock(Mutex* mu) EXCLUSIVE_LOCK_FUNCTION(mu) : mu_(mu) {
    this->mu_->Lock();
  }

  MutexLock(const MutexLock&) = delete;  // NOLINT(runtime/mutex)
  MutexLock(MutexLock&&) = delete;       // NOLINT(runtime/mutex)
  MutexLock& operator=(const MutexLock&) = delete;
  MutexLock& operator=(MutexLock&&) = delete;

  ~MutexLock() UNLOCK_FUNCTION() { this->mu_->Unlock(); }

 private:
  Mutex* const mu_;
};

// ReaderMutexLock
//
// The `ReaderMutexLock` is a helper class, like `MutexLock`, which acquires and
// releases a shared lock on a `Mutex` via RAII.
class SCOPED_LOCKABLE ReaderMutexLock {
 public:
  explicit ReaderMutexLock(Mutex* mu) SHARED_LOCK_FUNCTION(mu) : mu_(mu) {
    mu->ReaderLock();
  }

  ReaderMutexLock(const ReaderMutexLock&) = delete;
  ReaderMutexLock(ReaderMutexLock&&) = delete;
  ReaderMutexLock& operator=(const ReaderMutexLock&) = delete;
  ReaderMutexLock& operator=(ReaderMutexLock&&) = delete;

  ~ReaderMutexLock() UNLOCK_FUNCTION() { this->mu_->ReaderUnlock(); }

 private:
  Mutex* const mu_;
};

// WriterMutexLock
//
// The `WriterMutexLock` is a helper class, like `MutexLock`, which acquires and
// releases a write (exclusive) lock on a `Mutex` via RAII.
class SCOPED_LOCKABLE WriterMutexLock {
 public:
  explicit WriterMutexLock(Mutex* mu) EXCLUSIVE_LOCK_FUNCTION(mu) : mu_(mu) {
    mu->WriterLock();
  }

  WriterMutexLock(const WriterMutexLock&) = delete;
  WriterMutexLock(WriterMutexLock&&) = delete;
  WriterMutexLock& operator=(const WriterMutexLock&) = delete;
  WriterMutexLock& operator=(WriterMutexLock&&) = delete;

  ~WriterMutexLock() UNLOCK_FUNCTION() { this->mu_->WriterUnlock(); }

 private:
  Mutex* const mu_;
};

// -----------------------------------------------------------------------------
// Condition
// -----------------------------------------------------------------------------
//
// As noted above, `Mutex` contains a number of member functions which take a
// `Condition` as an argument; clients can wait for conditions to become `true`
// before attempting to acquire the mutex. These sections are known as
// "condition critical" sections. To use a `Condition`, you simply need to
// construct it, and use within an appropriate `Mutex` member function;
// everything else in the `Condition` class is an implementation detail.
//
// A `Condition` is specified as a function pointer which returns a boolean.
// `Condition` functions should be pure functions -- their results should depend
// only on passed arguments, should not consult any external state (such as
// clocks), and should have no side-effects, aside from debug logging. Any
// objects that the function may access should be limited to those which are
// constant while the mutex is blocked on the condition (e.g. a stack variable),
// or objects of state protected explicitly by the mutex.
//
// No matter which construction is used for `Condition`, the underlying
// function pointer / functor / callable must not throw any
// exceptions. Correctness of `Mutex` / `Condition` is not guaranteed in
// the face of a throwing `Condition`. (When Abseil is allowed to depend
// on C++17, these function pointers will be explicitly marked
// `noexcept`; until then this requirement cannot be enforced in the
// type system.)
//
// Note: to use a `Condition`, you need only construct it and pass it within the
// appropriate `Mutex' member function, such as `Mutex::Await()`.
//
// Example:
//
//   // assume count_ is not internal reference count
//   int count_ GUARDED_BY(mu_);
//
//   mu_.LockWhen(Condition(+[](int* count) { return *count == 0; },
//         &count_));
//
// When multiple threads are waiting on exactly the same condition, make sure
// that they are constructed with the same parameters (same pointer to function
// + arg, or same pointer to object + method), so that the mutex implementation
// can avoid redundantly evaluating the same condition for each thread.
class Condition {
 public:
  // A Condition that returns the result of "(*func)(arg)"
  Condition(bool (*func)(void*), void* arg);

  // Templated version for people who are averse to casts.
  //
  // To use a lambda, prepend it with unary plus, which converts the lambda
  // into a function pointer:
  //     Condition(+[](T* t) { return ...; }, arg).
  //
  // Note: lambdas in this case must contain no bound variables.
  //
  // See class comment for performance advice.
  template <typename T>
  Condition(bool (*func)(T*), T* arg);

  // Templated version for invoking a method that returns a `bool`.
  //
  // `Condition(object, &Class::Method)` constructs a `Condition` that evaluates
  // `object->Method()`.
  //
  // Implementation Note: `absl::internal::identity` is used to allow methods to
  // come from base classes. A simpler signature like
  // `Condition(T*, bool (T::*)())` does not suffice.
  template <typename T>
  Condition(T* object, bool (absl::internal::identity<T>::type::*method)());

  // Same as above, for const members
  template <typename T>
  Condition(const T* object,
            bool (absl::internal::identity<T>::type::*method)() const);

  // A Condition that returns the value of `*cond`
  explicit Condition(const bool* cond);

  // Templated version for invoking a functor that returns a `bool`.
  // This approach accepts pointers to non-mutable lambdas, `std::function`,
  // the result of` std::bind` and user-defined functors that define
  // `bool F::operator()() const`.
  //
  // Example:
  //
  //   auto reached = [this, current]() {
  //     mu_.AssertReaderHeld();                // For annotalysis.
  //     return processed_ >= current;
  //   };
  //   mu_.Await(Condition(&reached));

  // See class comment for performance advice. In particular, if there
  // might be more than one waiter for the same condition, make sure
  // that all waiters construct the condition with the same pointers.

  // Implementation note: The second template parameter ensures that this
  // constructor doesn't participate in overload resolution if T doesn't have
  // `bool operator() const`.
  template <typename T, typename E = decltype(
                            static_cast<bool (T::*)() const>(&T::operator()))>
  explicit Condition(const T* obj)
      : Condition(obj, static_cast<bool (T::*)() const>(&T::operator())) {}

  // A Condition that always returns `true`.
  static const Condition kTrue;

  // Evaluates the condition.
  bool Eval() const;

  // Returns `true` if the two conditions are guaranteed to return the same
  // value if evaluated at the same time, `false` if the evaluation *may* return
  // different results.
  //
  // Two `Condition` values are guaranteed equal if both their `func` and `arg`
  // components are the same. A null pointer is equivalent to a `true`
  // condition.
  static bool GuaranteedEqual(const Condition* a, const Condition* b);

 private:
  typedef bool (*InternalFunctionType)(void* arg);
  typedef bool (Condition::*InternalMethodType)();
  typedef bool (*InternalMethodCallerType)(void* arg,
                                           InternalMethodType internal_method);

  bool (*eval_)(const Condition*);  // Actual evaluator
  InternalFunctionType function_;   // function taking pointer returning bool
  InternalMethodType method_;       // method returning bool
  void* arg_;                       // arg of function_ or object of method_

  Condition();  // null constructor used only to create kTrue

  // Various functions eval_ can point to:
  static bool CallVoidPtrFunction(const Condition*);
  template <typename T>
  static bool CastAndCallFunction(const Condition* c);
  template <typename T>
  static bool CastAndCallMethod(const Condition* c);
};

// -----------------------------------------------------------------------------
// CondVar
// -----------------------------------------------------------------------------
//
// A condition variable, reflecting state evaluated separately outside of the
// `Mutex` object, which can be signaled to wake callers.
// This class is not normally needed; use `Mutex` member functions such as
// `Mutex::Await()` and intrinsic `Condition` abstractions. In rare cases
// with many threads and many conditions, `CondVar` may be faster.
//
// The implementation may deliver signals to any condition variable at
// any time, even when no call to `Signal()` or `SignalAll()` is made; as a
// result, upon being awoken, you must check the logical condition you have
// been waiting upon.
//
// Examples:
//
// Usage for a thread waiting for some condition C protected by mutex mu:
//       mu.Lock();
//       while (!C) { cv->Wait(&mu); }        // releases and reacquires mu
//       //  C holds; process data
//       mu.Unlock();
//
// Usage to wake T is:
//       mu.Lock();
//      // process data, possibly establishing C
//      if (C) { cv->Signal(); }
//      mu.Unlock();
//
// If C may be useful to more than one waiter, use `SignalAll()` instead of
// `Signal()`.
//
// With this implementation it is efficient to use `Signal()/SignalAll()` inside
// the locked region; this usage can make reasoning about your program easier.
//
class CondVar {
 public:
  CondVar();
  ~CondVar();

  // CondVar::Wait()
  //
  // Atomically releases a `Mutex` and blocks on this condition variable.
  // Waits until awakened by a call to `Signal()` or `SignalAll()` (or a
  // spurious wakeup), then reacquires the `Mutex` and returns.
  //
  // Requires and ensures that the current thread holds the `Mutex`.
  void Wait(Mutex* mu);

  // CondVar::WaitWithTimeout()
  //
  // Atomically releases a `Mutex` and blocks on this condition variable.
  // Waits until awakened by a call to `Signal()` or `SignalAll()` (or a
  // spurious wakeup), or until the timeout has expired, then reacquires
  // the `Mutex` and returns.
  //
  // Returns true if the timeout has expired without this `CondVar`
  // being signalled in any manner. If both the timeout has expired
  // and this `CondVar` has been signalled, the implementation is free
  // to return `true` or `false`.
  //
  // Requires and ensures that the current thread holds the `Mutex`.
  bool WaitWithTimeout(Mutex* mu, absl::Duration timeout);

  // CondVar::WaitWithDeadline()
  //
  // Atomically releases a `Mutex` and blocks on this condition variable.
  // Waits until awakened by a call to `Signal()` or `SignalAll()` (or a
  // spurious wakeup), or until the deadline has passed, then reacquires
  // the `Mutex` and returns.
  //
  // Deadlines in the past are equivalent to an immediate deadline.
  //
  // Returns true if the deadline has passed without this `CondVar`
  // being signalled in any manner. If both the deadline has passed
  // and this `CondVar` has been signalled, the implementation is free
  // to return `true` or `false`.
  //
  // Requires and ensures that the current thread holds the `Mutex`.
  bool WaitWithDeadline(Mutex* mu, absl::Time deadline);

  // CondVar::Signal()
  //
  // Signal this `CondVar`; wake at least one waiter if one exists.
  void Signal();

  // CondVar::SignalAll()
  //
  // Signal this `CondVar`; wake all waiters.
  void SignalAll();

  // CondVar::EnableDebugLog()
  //
  // Causes all subsequent uses of this `CondVar` to be logged via
  // `ABSL_RAW_LOG(INFO)`. Log entries are tagged with `name` if `name != 0`.
  // Note: this method substantially reduces `CondVar` performance.
  void EnableDebugLog(const char* name);

 private:
#ifdef ABSL_INTERNAL_USE_NONPROD_MUTEX
  synchronization_internal::CondVarImpl* impl() { return impl_.get(); }
  synchronization_internal::SynchronizationStorage<
      synchronization_internal::CondVarImpl>
      impl_;
#else
  bool WaitCommon(Mutex* mutex, synchronization_internal::KernelTimeout t);
  void Remove(base_internal::PerThreadSynch* s);
  void Wakeup(base_internal::PerThreadSynch* w);
  std::atomic<intptr_t> cv_;  // Condition variable state.
#endif
  CondVar(const CondVar&) = delete;
  CondVar& operator=(const CondVar&) = delete;
};

// Variants of MutexLock.
//
// If you find yourself using one of these, consider instead using
// Mutex::Unlock() and/or if-statements for clarity.

// MutexLockMaybe
//
// MutexLockMaybe is like MutexLock, but is a no-op when mu is null.
class SCOPED_LOCKABLE MutexLockMaybe {
 public:
  explicit MutexLockMaybe(Mutex* mu) EXCLUSIVE_LOCK_FUNCTION(mu) : mu_(mu) {
    if (this->mu_ != nullptr) {
      this->mu_->Lock();
    }
  }
  ~MutexLockMaybe() UNLOCK_FUNCTION() {
    if (this->mu_ != nullptr) {
      this->mu_->Unlock();
    }
  }

 private:
  Mutex* const mu_;
  MutexLockMaybe(const MutexLockMaybe&) = delete;
  MutexLockMaybe(MutexLockMaybe&&) = delete;
  MutexLockMaybe& operator=(const MutexLockMaybe&) = delete;
  MutexLockMaybe& operator=(MutexLockMaybe&&) = delete;
};

// ReleasableMutexLock
//
// ReleasableMutexLock is like MutexLock, but permits `Release()` of its
// mutex before destruction. `Release()` may be called at most once.
class SCOPED_LOCKABLE ReleasableMutexLock {
 public:
  explicit ReleasableMutexLock(Mutex* mu) EXCLUSIVE_LOCK_FUNCTION(mu)
      : mu_(mu) {
    this->mu_->Lock();
  }
  ~ReleasableMutexLock() UNLOCK_FUNCTION() {
    if (this->mu_ != nullptr) {
      this->mu_->Unlock();
    }
  }

  void Release() UNLOCK_FUNCTION();

 private:
  Mutex* mu_;
  ReleasableMutexLock(const ReleasableMutexLock&) = delete;
  ReleasableMutexLock(ReleasableMutexLock&&) = delete;
  ReleasableMutexLock& operator=(const ReleasableMutexLock&) = delete;
  ReleasableMutexLock& operator=(ReleasableMutexLock&&) = delete;
};

#ifdef ABSL_INTERNAL_USE_NONPROD_MUTEX
#else
inline Mutex::Mutex() : mu_(0) {
  ABSL_TSAN_MUTEX_CREATE(this, __tsan_mutex_not_static);
}

inline CondVar::CondVar() : cv_(0) {}
#endif

// static
template <typename T>
bool Condition::CastAndCallMethod(const Condition* c) {
  typedef bool (T::*MemberType)();
  MemberType rm = reinterpret_cast<MemberType>(c->method_);
  T* x = static_cast<T*>(c->arg_);
  return (x->*rm)();
}

// static
template <typename T>
bool Condition::CastAndCallFunction(const Condition* c) {
  typedef bool (*FuncType)(T*);
  FuncType fn = reinterpret_cast<FuncType>(c->function_);
  T* x = static_cast<T*>(c->arg_);
  return (*fn)(x);
}

template <typename T>
inline Condition::Condition(bool (*func)(T*), T* arg)
    : eval_(&CastAndCallFunction<T>),
      function_(reinterpret_cast<InternalFunctionType>(func)),
      method_(nullptr),
      arg_(const_cast<void*>(static_cast<const void*>(arg))) {}

template <typename T>
inline Condition::Condition(T* object,
                            bool (absl::internal::identity<T>::type::*method)())
    : eval_(&CastAndCallMethod<T>),
      function_(nullptr),
      method_(reinterpret_cast<InternalMethodType>(method)),
      arg_(object) {}

template <typename T>
inline Condition::Condition(const T* object,
                            bool (absl::internal::identity<T>::type::*method)()
                                const)
    : eval_(&CastAndCallMethod<T>),
      function_(nullptr),
      method_(reinterpret_cast<InternalMethodType>(method)),
      arg_(reinterpret_cast<void*>(const_cast<T*>(object))) {}

// Register a hook for profiling support.
//
// The function pointer registered here will be called whenever a mutex is
// contended.  The callback is given the absl/base/cycleclock.h timestamp when
// waiting began.
//
// Calls to this function do not race or block, but there is no ordering
// guaranteed between calls to this function and call to the provided hook.
// In particular, the previously registered hook may still be called for some
// time after this function returns.
void RegisterMutexProfiler(void (*fn)(int64_t wait_timestamp));

// Register a hook for Mutex tracing.
//
// The function pointer registered here will be called whenever a mutex is
// contended.  The callback is given an opaque handle to the contended mutex,
// an event name, and the number of wait cycles (as measured by
// //absl/base/internal/cycleclock.h, and which may not be real
// "cycle" counts.)
//
// The only event name currently sent is "slow release".
//
// This has the same memory ordering concerns as RegisterMutexProfiler() above.
void RegisterMutexTracer(void (*fn)(const char* msg, const void* obj,
                                    int64_t wait_cycles));

// TODO(gfalcon): Combine RegisterMutexProfiler() and RegisterMutexTracer()
// into a single interface, since they are only ever called in pairs.

// Register a hook for CondVar tracing.
//
// The function pointer registered here will be called here on various CondVar
// events.  The callback is given an opaque handle to the CondVar object and
// a string identifying the event.  This is thread-safe, but only a single
// tracer can be registered.
//
// Events that can be sent are "Wait", "Unwait", "Signal wakeup", and
// "SignalAll wakeup".
//
// This has the same memory ordering concerns as RegisterMutexProfiler() above.
void RegisterCondVarTracer(void (*fn)(const char* msg, const void* cv));

// Register a hook for symbolizing stack traces in deadlock detector reports.
//
// 'pc' is the program counter being symbolized, 'out' is the buffer to write
// into, and 'out_size' is the size of the buffer.  This function can return
// false if symbolizing failed, or true if a null-terminated symbol was written
// to 'out.'
//
// This has the same memory ordering concerns as RegisterMutexProfiler() above.
//
// DEPRECATED: The default symbolizer function is absl::Symbolize() and the
// ability to register a different hook for symbolizing stack traces will be
// removed on or after 2023-05-01.
ABSL_DEPRECATED(
    "absl::RegisterSymbolizer() is deprecated and will be removed "
    "on or after 2023-05-01")
void RegisterSymbolizer(bool (*fn)(const void* pc, char* out, int out_size));

// EnableMutexInvariantDebugging()
//
// Enable or disable global support for Mutex invariant debugging.  If enabled,
// then invariant predicates can be registered per-Mutex for debug checking.
// See Mutex::EnableInvariantDebugging().
void EnableMutexInvariantDebugging(bool enabled);

// When in debug mode, and when the feature has been enabled globally, the
// implementation will keep track of lock ordering and complain (or optionally
// crash) if a cycle is detected in the acquired-before graph.

// Possible modes of operation for the deadlock detector in debug mode.
enum class OnDeadlockCycle {
  kIgnore,  // Neither report on nor attempt to track cycles in lock ordering
  kReport,  // Report lock cycles to stderr when detected
  kAbort,   // Report lock cycles to stderr when detected, then abort
};

// SetMutexDeadlockDetectionMode()
//
// Enable or disable global support for detection of potential deadlocks
// due to Mutex lock ordering inversions.  When set to 'kIgnore', tracking of
// lock ordering is disabled.  Otherwise, in debug builds, a lock ordering graph
// will be maintained internally, and detected cycles will be reported in
// the manner chosen here.
void SetMutexDeadlockDetectionMode(OnDeadlockCycle mode);

}  // namespace absl

// In some build configurations we pass --detect-odr-violations to the
// gold linker.  This causes it to flag weak symbol overrides as ODR
// violations.  Because ODR only applies to C++ and not C,
// --detect-odr-violations ignores symbols not mangled with C++ names.
// By changing our extension points to be extern "C", we dodge this
// check.
extern "C" {
void AbslInternalMutexYield();
}  // extern "C"
#endif  // ABSL_SYNCHRONIZATION_MUTEX_H_
