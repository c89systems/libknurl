#ifndef ROTMOS_LOOP_H
#define ROTMOS_LOOP_H

/** @file loop.h Public loop API. Expected failures return rotmos_status and
 * optional bounded rotmos_error diagnostics. Take operations transfer
 * ownership only on success; views remain borrowed for their documented scope.
 */

#include <rotmos/common.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Read readiness event/interest bit. */
#define ROTMOS_READABLE 1U
/** Write readiness event/interest bit. */
#define ROTMOS_WRITABLE 2U
/** Read-half closure notification bit. */
#define ROTMOS_READ_CLOSED 4U
/** Descriptor error notification bit. */
#define ROTMOS_EVENT_ERROR 8U

/** Maximum live watch/timer/waker/deferred registrations per loop: 4096;
 * retirement also consumes a callback slot.
 */
#define ROTMOS_LOOP_MAX_REGISTRATIONS 4096U
/** Maximum queued deferred deliveries per loop: 1024. */
#define ROTMOS_LOOP_MAX_DEFERRED 1024U
/** Maximum delivery/retirement callbacks per owner-loop turn: 64. */
#define ROTMOS_LOOP_CALLBACKS_PER_TURN 64U

/** Actual resource/registration retirement notification; release caller context
 * here after all retained work acknowledges.
 */
typedef void (*rotmos_retire_fn)(void *userdata);
/** Owner-loop borrowed-FD readiness callback. Ready flags use ROTMOS_* bits;
 * never close/reuse the FD before watch cancellation.
 */
typedef void (*rotmos_watch_fn)(rotmos_watch *watch, unsigned int ready_flags,
                                void *userdata);
/** Owner-loop timer callback; timer/userdata remain valid under the
 * registration retirement contract.
 */
typedef void (*rotmos_timer_fn)(rotmos_timer *timer, void *userdata);
/** One later owner-loop delivery; no synchronous callback during registration.
 */
typedef void (*rotmos_defer_fn)(rotmos_loop *loop, void *userdata);
/** Owner-loop delivery of one or more coalesced cross-thread signals. */
typedef void (*rotmos_waker_fn)(rotmos_loop *loop, void *userdata);

/** Borrow the current process's loop during an active server run. Captured
 * handles cannot cross a fork or run boundary. All operations require the
 * owner thread except waker_signal and server_request_stop.
 */
ROTMOS_API rotmos_loop *rotmos_server_loop(rotmos_server *server);
/** Read the active loop's exact monotonic millisecond clock; owner thread only.
 */
ROTMOS_API rotmos_status rotmos_loop_now(const rotmos_loop *loop,
                                         rotmos_u64 *out, rotmos_error *error);

/** The descriptor is borrowed, never closed or made nonblocking. Cancel before
 * closing/reusing it. A cancelled watch permits a replacement registration
 * before its retirement callback; its old userdata stays alive until
 * retirement. A zero interest suspends delivery. New registrations own one
 * retirement notification only on success.
 */
ROTMOS_API rotmos_status rotmos_loop_watch(rotmos_loop *loop, int fd,
                                           unsigned int interest,
                                           rotmos_watch_fn callback,
                                           rotmos_retire_fn retired,
                                           void *userdata, rotmos_watch **out,
                                           rotmos_error *error);
/** Update a live borrowed-FD watch's interest mask; zero suspends delivery. */
ROTMOS_API rotmos_status rotmos_watch_update(rotmos_watch *watch,
                                             unsigned int interest,
                                             rotmos_error *error);
/** Cancel the watch on its owner loop; keep userdata until retirement and
 * cancel before closing/reusing its FD.
 */
ROTMOS_API void rotmos_watch_cancel(rotmos_watch *watch);
/** Register an owner-loop timer in milliseconds; repeat_ms zero is one-shot.
 * Success owns one retirement notification.
 */
ROTMOS_API rotmos_status rotmos_loop_timer(rotmos_loop *loop, long delay_ms,
                                           long repeat_ms,
                                           rotmos_timer_fn callback,
                                           rotmos_retire_fn retired,
                                           void *userdata, rotmos_timer **out,
                                           rotmos_error *error);
/** Cancel an owner-loop timer; userdata remains alive until retirement. */
ROTMOS_API void rotmos_timer_cancel(rotmos_timer *timer);
/** Schedule one later owner-loop callback; success owns one retirement
 * notification. Never calls inline.
 */
ROTMOS_API rotmos_status rotmos_loop_defer(rotmos_loop *loop,
                                           rotmos_defer_fn callback,
                                           rotmos_retire_fn retired,
                                           void *userdata, rotmos_error *error);

/** Create an owned cross-thread notification token; delivery/retirement run on
 * its owner loop.
 */
ROTMOS_API rotmos_status rotmos_waker_new(rotmos_loop *loop,
                                          rotmos_waker_fn callback,
                                          rotmos_retire_fn retired,
                                          void *userdata, rotmos_waker **out,
                                          rotmos_error *error);
/** Signals coalesce through the loop's real wake descriptor. Stop/join all
 * producers before destroy; no pointer may be signalled after retirement.
 */
ROTMOS_API rotmos_status rotmos_waker_signal(rotmos_waker *waker,
                                             rotmos_error *error);
/** Retire a waker on its owner loop after all signalling producers have
 * stopped.
 */
ROTMOS_API rotmos_status rotmos_waker_destroy(rotmos_waker *waker,
                                              rotmos_error *error);

#ifdef __cplusplus
}
#endif
#endif
