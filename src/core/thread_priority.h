#pragma once

#if defined(__APPLE__)
#include <pthread/qos.h>
#endif

namespace kestrel {

// How urgent a thread's work is. On Apple Silicon this decides whether macOS runs it
// on performance or efficiency cores.
enum class ThreadRole {
  kInteractive,  // guest CPUs and the GPU: frames and input depend on them
  kIo,           // disk and network: important, but not every frame
};

inline void set_thread_role(ThreadRole role) {
#if defined(__APPLE__)
  pthread_set_qos_class_self_np(role == ThreadRole::kInteractive ? QOS_CLASS_USER_INTERACTIVE
                                                                 : QOS_CLASS_USER_INITIATED,
                                0);
#else
  (void)role;
#endif
}

}  // namespace kestrel
