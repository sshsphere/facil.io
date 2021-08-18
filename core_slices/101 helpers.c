/* *****************************************************************************
Internal helpers
***************************************************************************** */
#ifndef H_FACIL_IO_H /* development sugar, ignore */
#include "999 dev.h" /* development sugar, ignore */
#endif               /* development sugar, ignore */

#define FIO_STREAM
#include "fio-stl.h"

#define FIO_MALLOC_TMP_USE_SYSTEM
#define FIO_QUEUE
#define FIO_SIGNAL
#define FIO_SOCK /* should be public? */
#include "fio-stl.h"

/* *****************************************************************************
Polling Helpers
***************************************************************************** */

#if !defined(FIO_ENGINE_EPOLL) && !defined(FIO_ENGINE_KQUEUE) &&               \
    !defined(FIO_ENGINE_POLL)
#if defined(HAVE_EPOLL) || __has_include("sys/epoll.h")
#define FIO_ENGINE_EPOLL 1
#elif (defined(HAVE_KQUEUE) || __has_include("sys/event.h"))
#define FIO_ENGINE_KQUEUE 1
#else
#define FIO_ENGINE_POLL 1
#endif
#endif /* !defined(FIO_ENGINE_EPOLL) ... */

#ifndef FIO_POLL_TICK
#define FIO_POLL_TICK 1000
#endif

#ifndef FIO_POLL_SHUTDOWN_TICK
#define FIO_POLL_SHUTDOWN_TICK 10
#endif

FIO_SFUNC void fio_monitor_init(void);
FIO_SFUNC void fio_monitor_destroy(void);
FIO_IFUNC void fio_monitor_monitor(int fd, void *udata, uintptr_t flags);
FIO_SFUNC void fio_monitor_forget(int fd);
FIO_SFUNC int fio_monitor_review(int timeout);

FIO_IFUNC void fio_monitor_read(fio_s *io);
FIO_IFUNC void fio_monitor_write(fio_s *io);
FIO_IFUNC void fio_monitor_all(fio_s *io);

/* *****************************************************************************
Queue Helpers
***************************************************************************** */

FIO_IFUNC fio_queue_s *fio_queue_select(uintptr_t flag);

#define FIO_QUEUE_SYSTEM fio_queue_select(1)
#define FIO_QUEUE_USER   fio_queue_select(0)

/* *****************************************************************************
ENV data maps (must be defined before the IO object that owns them)
***************************************************************************** */

/** An object that can be linked to any facil.io connection (fio_s). */
typedef struct {
  void (*on_close)(void *data);
  void *udata;
} env_obj_s;

/* cleanup event task */
static void env_obj_call_callback_task(void *p, void *udata) {
  union {
    void (*fn)(void *);
    void *p;
  } u = {.p = p};
  u.fn(udata);
}

/* cleanup event scheduling */
FIO_IFUNC void env_obj_call_callback(env_obj_s o) {
  union {
    void (*fn)(void *);
    void *p;
  } u = {.fn = o.on_close};
  if (o.on_close) {
    fio_queue_push_urgent(FIO_QUEUE_USER,
                          env_obj_call_callback_task,
                          u.p,
                          o.udata);
  }
}

/* for storing ENV string keys */
#define FIO_STR_SMALL sstr
#include "fio-stl.h"

#define FIO_UMAP_NAME              env
#define FIO_MAP_TYPE               env_obj_s
#define FIO_MAP_TYPE_DESTROY(o)    env_obj_call_callback((o))
#define FIO_MAP_DESTROY_AFTER_COPY 0

/* destroy discarded keys when overwriting existing data (const_name support) */
#define FIO_MAP_KEY                 sstr_s /* the small string type */
#define FIO_MAP_KEY_COPY(dest, src) (dest) = (src)
#define FIO_MAP_KEY_DESTROY(k)      sstr_destroy(&k)
#define FIO_MAP_KEY_DISCARD         FIO_MAP_KEY_DESTROY
#define FIO_MAP_KEY_CMP(a, b)       sstr_is_eq(&(a), &(b))
#include <fio-stl.h>

/* *****************************************************************************
CPU Core Counting
***************************************************************************** */

static inline size_t fio_detect_cpu_cores(void) {
  ssize_t cpu_count = 0;
#ifdef _SC_NPROCESSORS_ONLN
  cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
  if (cpu_count < 0) {
    FIO_LOG_WARNING("CPU core count auto-detection failed.");
    return 0;
  }
#else
  FIO_LOG_WARNING("CPU core count auto-detection failed.");
  cpu_count = FIO_CPU_CORES_FALLBACK;
#endif
  return cpu_count;
}

/* *****************************************************************************
Event deferring (declarations)
***************************************************************************** */

static void fio_ev_on_ready(void *io, void *udata);
static void fio_ev_on_ready_user(void *io, void *udata);
static void fio_ev_on_data(void *io, void *udata);
static void fio_ev_on_timeout(void *io, void *udata);
static void fio_ev_on_shutdown(void *io, void *udata);
static void fio_ev_on_close(void *io, void *udata);

static void mock_ping_eternal(fio_s *io);

/** Points to a function that keeps the connection alive. */
void (*FIO_PING_ETERNAL)(fio_s *) = mock_ping_eternal;

/* *****************************************************************************
Event deferring (mock functions)
***************************************************************************** */

static void mock_on_data(fio_s *io) { fio_suspend(io); }
static void mock_on_ready(fio_s *io) { (void)io; }
static void mock_on_shutdown(fio_s *io) { (void)io; }
static void mock_on_close(void *udata) { (void)udata; }
static void mock_timeout(fio_s *io) {
  fio_close_now(io);
  (void)io;
}
static void mock_ping_eternal(fio_s *io) { fio_touch(io); }

FIO_IFUNC void fio_protocol_validate(fio_protocol_s *p) {
  if (p && !(p->reserved.flags & 8)) {
    if (!p->on_data)
      p->on_data = mock_on_data;
    if (!p->on_timeout)
      p->on_timeout = mock_timeout;
    if (!p->on_ready)
      p->on_ready = mock_on_ready;
    if (!p->on_shutdown)
      p->on_shutdown = mock_on_shutdown;
    if (!p->on_close)
      p->on_close = mock_on_close;
    p->reserved.flags |= 8;
  }
}

/* *****************************************************************************
Child Reaping
***************************************************************************** */

/* reap children handler. */
static void fio___reap_children(int sig, void *ignr_) {
#if FIO_OS_POSIX
  FIO_ASSERT_DEBUG(sig == SIGCHLD, "wrong signal handler called");
  while (waitpid(-1, &sig, WNOHANG) > 0)
    ;
#else
  (void)sig;
  FIO_ASSERT(0, "Children reaping is only supported on POSIX systems.");
#endif
  (void)ignr_;
}
/**
 * Initializes zombie reaping for the process. Call before `fio_start` to enable
 * global zombie reaping.
 */
void fio_reap_children(void) {
#if FIO_OS_POSIX
  fio_signal_monitor(SIGCHLD, fio___reap_children, NULL);
#else
  (void)fio___reap_children;
  FIO_LOG_ERROR("fio_reap_children unsupported on this system.");
#endif
}
