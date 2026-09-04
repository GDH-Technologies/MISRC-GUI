/*
 * MISRC GUI - Portable mutex / condition-variable shims for the networking
 * module. gui_net.c and gui_net_fanout.c share these so the fanout can be
 * compiled and exercised on its own (misrc_tools/test/net_fanout_harness.c).
 */
#ifndef GUI_NET_SYNC_H
#define GUI_NET_SYNC_H

#ifdef _WIN32
  /* Avoid Win32 API name collisions with raylib symbols (Rectangle, CloseWindow, ShowCursor). */
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOGDI
  #define NOGDI
  #endif
  #ifndef NOUSER
  #define NOUSER
  #endif
  #include <windows.h>
  typedef CRITICAL_SECTION net_mutex_t;
  typedef CONDITION_VARIABLE net_cond_t;
  #define net_mutex_init(m)     InitializeCriticalSection(m)
  #define net_mutex_destroy(m)  DeleteCriticalSection(m)
  #define net_mutex_lock(m)     EnterCriticalSection(m)
  #define net_mutex_unlock(m)   LeaveCriticalSection(m)
  #define net_cond_init(c)      InitializeConditionVariable(c)
  #define net_cond_destroy(c)   ((void)0)
  #define net_cond_broadcast(c) WakeAllConditionVariable(c)
  /* Wait on c with m held. Returns 0 when signalled, -1 on timeout. */
  static inline int net_cond_timedwait_ms(net_cond_t *c, net_mutex_t *m, int timeout_ms) {
      return SleepConditionVariableCS(c, m, (DWORD)timeout_ms) ? 0 : -1;
  }
#else
  #include <errno.h>
  #include <pthread.h>
  #include <time.h>
  typedef pthread_mutex_t net_mutex_t;
  typedef pthread_cond_t net_cond_t;
  #define net_mutex_init(m)     ((void)pthread_mutex_init(m, NULL))
  #define net_mutex_destroy(m)  ((void)pthread_mutex_destroy(m))
  #define net_mutex_lock(m)     ((void)pthread_mutex_lock(m))
  #define net_mutex_unlock(m)   ((void)pthread_mutex_unlock(m))
  #define net_cond_init(c)      ((void)pthread_cond_init(c, NULL))
  #define net_cond_destroy(c)   ((void)pthread_cond_destroy(c))
  #define net_cond_broadcast(c) ((void)pthread_cond_broadcast(c))
  /* Wait on c with m held. Returns 0 when signalled, -1 on timeout. */
  static inline int net_cond_timedwait_ms(net_cond_t *c, net_mutex_t *m, int timeout_ms) {
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_sec += timeout_ms / 1000;
      ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
      if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
      return (pthread_cond_timedwait(c, m, &ts) == ETIMEDOUT) ? -1 : 0;
  }
#endif

#endif /* GUI_NET_SYNC_H */
