#ifndef ROTMOS_LOOP_H
#define ROTMOS_LOOP_H

#include <rotmos/common.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ROTMOS_READABLE 1U
#define ROTMOS_WRITABLE 2U
#define ROTMOS_READ_CLOSED 4U
#define ROTMOS_EVENT_ERROR 8U

/* Per-loop finite bounds; registrations include watches, timers, wakers and
 * deferred items. Each delivery or retirement consumes a callback turn slot.
 */
#define ROTMOS_LOOP_MAX_REGISTRATIONS 4096U
#define ROTMOS_LOOP_MAX_DEFERRED 1024U
#define ROTMOS_LOOP_CALLBACKS_PER_TURN 64U

typedef void (*rotmos_retire_fn)(void *userdata);
typedef void (*rotmos_watch_fn)(rotmos_watch *watch, unsigned int ready_flags,
                                void *userdata);
typedef void (*rotmos_timer_fn)(rotmos_timer *timer, void *userdata);
typedef void (*rotmos_defer_fn)(rotmos_loop *loop, void *userdata);
typedef void (*rotmos_waker_fn)(rotmos_loop *loop, void *userdata);

/* Borrow the current process's loop during an active server run. Captured
 * handles cannot cross a fork or run boundary. All operations require the
 * owner thread except waker_signal and server_request_stop.
 */
ROTMOS_API rotmos_loop *rotmos_server_loop(rotmos_server *server);
ROTMOS_API rotmos_status rotmos_loop_now(const rotmos_loop *loop,
                                         rotmos_u64 *out, rotmos_error *error);

/* The descriptor is borrowed, never closed or made nonblocking. Cancel before
 * closing/reusing it. A zero interest suspends delivery. New registrations
 * own one retirement notification only on success.
 */
ROTMOS_API rotmos_status rotmos_loop_watch(rotmos_loop *loop, int fd,
                                           unsigned int interest,
                                           rotmos_watch_fn callback,
                                           rotmos_retire_fn retired,
                                           void *userdata, rotmos_watch **out,
                                           rotmos_error *error);
ROTMOS_API rotmos_status rotmos_watch_update(rotmos_watch *watch,
                                             unsigned int interest,
                                             rotmos_error *error);
ROTMOS_API void rotmos_watch_cancel(rotmos_watch *watch);
ROTMOS_API rotmos_status rotmos_loop_timer(rotmos_loop *loop, long delay_ms,
                                           long repeat_ms,
                                           rotmos_timer_fn callback,
                                           rotmos_retire_fn retired,
                                           void *userdata, rotmos_timer **out,
                                           rotmos_error *error);
ROTMOS_API void rotmos_timer_cancel(rotmos_timer *timer);
ROTMOS_API rotmos_status rotmos_loop_defer(rotmos_loop *loop,
                                           rotmos_defer_fn callback,
                                           rotmos_retire_fn retired,
                                           void *userdata, rotmos_error *error);

ROTMOS_API rotmos_status rotmos_waker_new(rotmos_loop *loop,
                                          rotmos_waker_fn callback,
                                          rotmos_retire_fn retired,
                                          void *userdata, rotmos_waker **out,
                                          rotmos_error *error);
/* Signals coalesce through the loop's real wake descriptor. Stop/join all
 * producers before destroy; no pointer may be signalled after retirement.
 */
ROTMOS_API rotmos_status rotmos_waker_signal(rotmos_waker *waker,
                                             rotmos_error *error);
ROTMOS_API rotmos_status rotmos_waker_destroy(rotmos_waker *waker,
                                              rotmos_error *error);

#ifdef __cplusplus
}
#endif
#endif
