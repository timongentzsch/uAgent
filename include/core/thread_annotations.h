// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_THREAD_ANNOTATIONS_H_
#define UAGENT_INCLUDE_CORE_THREAD_ANNOTATIONS_H_
// Thread-safety annotations are on for Clang + libc++, whose std::mutex and
// std::lock_guard carry the capability annotations (CMake defines the
// switches). Other toolchains expand every macro here to nothing. See
// https://clang.llvm.org/docs/ThreadSafetyAnalysis.html.

#include <mutex>

#if defined(__clang__) && defined(UAGENT_ENABLE_THREAD_SAFETY_ANALYSIS)
#define UAGENT_TSA(annotation) __attribute__((annotation))
#else
#define UAGENT_TSA(annotation)
#endif

// The member may only be touched with `mutex` held.
#define UAGENT_GUARDED_BY(mutex) UAGENT_TSA(guarded_by(mutex))
// The caller must already hold the listed mutexes.
#define UAGENT_REQUIRES(...) UAGENT_TSA(requires_capability(__VA_ARGS__))

namespace uagent {

// libc++ annotates std::lock_guard but not std::unique_lock, whose lock() and
// unlock() the analysis cannot follow. This is std::unique_lock with the
// annotations, for scopes that wait on a condition variable or release the
// mutex early. Pass it to condition_variable::wait as the std::unique_lock it
// is.
class UAGENT_TSA(scoped_lockable) UniqueLock
    : public std::unique_lock<std::mutex> {
 public:
  explicit UniqueLock(std::mutex& mutex) UAGENT_TSA(acquire_capability(mutex))
      : std::unique_lock<std::mutex>(mutex) {}
  ~UniqueLock() UAGENT_TSA(release_capability()) = default;
  UniqueLock(const UniqueLock&) = delete;
  UniqueLock& operator=(const UniqueLock&) = delete;
  void lock() UAGENT_TSA(acquire_capability()) {
    std::unique_lock<std::mutex>::lock();
  }
  void unlock() UAGENT_TSA(release_capability()) {
    std::unique_lock<std::mutex>::unlock();
  }
};

}  // namespace uagent

#endif  // UAGENT_INCLUDE_CORE_THREAD_ANNOTATIONS_H_
