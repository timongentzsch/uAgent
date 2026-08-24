// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_INCLUDE_CORE_THREAD_ANNOTATIONS_H_
#define UAGENT_INCLUDE_CORE_THREAD_ANNOTATIONS_H_
// Thread-safety annotations are opt-in until the mutex type itself carries a
// Clang capability annotation; std::mutex does not portably provide one. See
// https://clang.llvm.org/docs/ThreadSafetyAnalysis.html.

#if defined(__clang__) && defined(UAGENT_ENABLE_THREAD_SAFETY_ANALYSIS)
#define UAGENT_TSA(annotation) __attribute__((annotation))
#else
#define UAGENT_TSA(annotation)
#endif

// The member may only be touched with `mutex` held.
#define UAGENT_GUARDED_BY(mutex) UAGENT_TSA(guarded_by(mutex))
// The caller must already hold the listed mutexes.
#define UAGENT_REQUIRES(...) UAGENT_TSA(requires_capability(__VA_ARGS__))
// For a body the analysis cannot follow. Every use needs a comment saying
// what makes it safe.
#define UAGENT_NO_TSA UAGENT_TSA(no_thread_safety_analysis)

#endif  // UAGENT_INCLUDE_CORE_THREAD_ANNOTATIONS_H_
