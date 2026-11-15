#ifndef KNURL_LOOP_H
#define KNURL_LOOP_H

/** @file loop.h Public loop API. Expected failures return knurl_status and
 * optional bounded knurl_error diagnostics. Take operations transfer
 * ownership only on success; views remain borrowed for their documented scope.
 */

#include <knurl/common.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Read readiness event/interest bit. */
#define KNURL_READABLE 1U
/** Write readiness event/interest bit. */
#define KNURL_WRITABLE 2U
/** Read-half closure notification bit. */
#define KNURL_READ_CLOSED 4U
/** Descriptor error notification bit. */
#define KNURL_EVENT_ERROR 8U

/** Maximum live watch/timer/waker/deferred registrations per loop: 4096;
 * retirement also consumes a callback slot.
 */
#define KNURL_LOOP_MAX_REGISTRATIONS 4096U
/** Maximum queued deferred deliveries per loop: 1024. */
#define KNURL_LOOP_MAX_DEFERRED 1024U
/** Maximum delivery/retirement callbacks per owner-loop turn: 64. */
#define KNURL_LOOP_CALLBACKS_PER_TURN 64U

/** Actual resource/registration retirement notification; release caller context
 * here after all retained work acknowledges.
 */
typedef void (*knurl_retire_fn)(void *userdata);
/** Owner-loop borrowed-FD readiness callback. Ready flags use KNURL_* bits;
 * never close/reuse the FD before watch cancellation.
 */
typedef void (*knurl_watch_fn)(knurl_watch *watch, unsigned int ready_flags,
                               void *userdata);
/** Owner-loop timer callback; timer/userdata remain valid under the
 * registration retirement contract.
 */
typedef void (*knurl_timer_fn)(knurl_timer *timer, void *userdata);
/** One later owner-loop delivery; no synchronous callback during registration.
 */
typedef void (*knurl_defer_fn)(knurl_loop *loop, void *userdata);
/** Owner-loop delivery of one or more coalesced cross-thread signals. */
typedef void (*knurl_waker_fn)(knurl_loop *loop, void *userdata);

/** Borrow the current process's loop during an active server run. Captured
 * handles cannot cross a fork or run boundary. All operations require the
 * owner thread except waker_signal and server_request_stop.
 */
KNURL_API knurl_loop *knurl_server_loop(knurl_server *server);
/** Read the active loop's exact monotonic millisecond clock; owner thread only.
 */
KNURL_API knurl_status knurl_loop_now(const knurl_loop *loop, knurl_u64 *out,
                                      knurl_error *error);

/** The descriptor is borrowed, never closed or made nonblocking. Cancel before
 * closing/reusing it. A cancelled watch permits a replacement registration
 * before its retirement callback; its old userdata stays alive until
 * retirement. A zero interest suspends delivery. New registrations own one
 * retirement notification only on success.
 */
KNURL_API knurl_status knurl_loop_watch(knurl_loop *loop, int fd,
                                        unsigned int interest,
                                        knurl_watch_fn callback,
                                        knurl_retire_fn retired, void *userdata,
                                        knurl_watch **out, knurl_error *error);
/** Update a live borrowed-FD watch's interest mask; zero suspends delivery. */
KNURL_API knurl_status knurl_watch_update(knurl_watch *watch,
                                          unsigned int interest,
                                          knurl_error *error);
/** Cancel the watch on its owner loop; keep userdata until retirement and
 * cancel before closing/reusing its FD.
 */
KNURL_API void knurl_watch_cancel(knurl_watch *watch);
/** Register an owner-loop timer in milliseconds; repeat_ms zero is one-shot.
 * Success owns one retirement notification.
 */
KNURL_API knurl_status knurl_loop_timer(knurl_loop *loop, long delay_ms,
                                        long repeat_ms, knurl_timer_fn callback,
                                        knurl_retire_fn retired, void *userdata,
                                        knurl_timer **out, knurl_error *error);
/** Cancel an owner-loop timer; userdata remains alive until retirement. */
KNURL_API void knurl_timer_cancel(knurl_timer *timer);
/** Schedule one later owner-loop callback; success owns one retirement
 * notification. Never calls inline.
 */
KNURL_API knurl_status knurl_loop_defer(knurl_loop *loop,
                                        knurl_defer_fn callback,
                                        knurl_retire_fn retired, void *userdata,
                                        knurl_error *error);

/** Create an owned cross-thread notification token; delivery/retirement run on
 * its owner loop.
 */
KNURL_API knurl_status knurl_waker_new(knurl_loop *loop,
                                       knurl_waker_fn callback,
                                       knurl_retire_fn retired, void *userdata,
                                       knurl_waker **out, knurl_error *error);
/** Signals coalesce through the loop's real wake descriptor. Stop/join all
 * producers before destroy; no pointer may be signalled after retirement.
 */
KNURL_API knurl_status knurl_waker_signal(knurl_waker *waker,
                                          knurl_error *error);
/** Retire a waker on its owner loop after all signalling producers have
 * stopped.
 */
KNURL_API knurl_status knurl_waker_destroy(knurl_waker *waker,
                                           knurl_error *error);

struct knurl_error;
struct knurl_timer;
struct knurl_u64;
struct knurl_waker;
struct knurl_watch;
/** Library-owned loop receiver. Pass this handle as the first
 * argument to its methods. Obtain it through the documented constructor or
 * borrowed accessor. Do not copy, initialize or modify the shell or impl.
 */
struct knurl_loop {
    /** Read the active loop's exact monotonic millisecond clock; owner thread
     * only.
     */
    knurl_status (*now)(const struct knurl_loop *loop, struct knurl_u64 *out,
                        struct knurl_error *error);
    /** The descriptor is borrowed, never closed or made nonblocking. Cancel
     * before closing/reusing it. A cancelled watch permits a replacement
     * registration before its retirement callback; its old userdata stays alive
     * until retirement. A zero interest suspends delivery. New registrations
     * own one retirement notification only on success.
     */
    knurl_status (*watch)(struct knurl_loop *loop, int fd,
                          unsigned int interest, knurl_watch_fn callback,
                          knurl_retire_fn retired, void *userdata,
                          struct knurl_watch **out, struct knurl_error *error);
    /** Register an owner-loop timer in milliseconds; repeat_ms zero is
     * one-shot. Success owns one retirement notification.
     */
    knurl_status (*timer)(struct knurl_loop *loop, long delay_ms,
                          long repeat_ms, knurl_timer_fn callback,
                          knurl_retire_fn retired, void *userdata,
                          struct knurl_timer **out, struct knurl_error *error);
    /** Schedule one later owner-loop callback; success owns one retirement
     * notification. Never calls inline.
     */
    knurl_status (*defer)(struct knurl_loop *loop, knurl_defer_fn callback,
                          knurl_retire_fn retired, void *userdata,
                          struct knurl_error *error);
    /** Create an owned cross-thread notification token; delivery/retirement run
     * on its owner loop.
     */
    knurl_status (*waker)(struct knurl_loop *loop, knurl_waker_fn callback,
                          knurl_retire_fn retired, void *userdata,
                          struct knurl_waker **out, struct knurl_error *error);
    /** Private library state; applications must not access it. */
    struct knurl_loop_impl *impl;
};

struct knurl_error;
/** Library-owned watch receiver. Pass this handle as the first
 * argument to its methods. Obtain it through the documented constructor or
 * borrowed accessor. Do not copy, initialize or modify the shell or impl.
 */
struct knurl_watch {
    /** Update a live borrowed-FD watch's interest mask; zero suspends delivery.
     */
    knurl_status (*update)(struct knurl_watch *watch, unsigned int interest,
                           struct knurl_error *error);
    /** Cancel the watch on its owner loop; keep userdata until retirement and
     * cancel before closing/reusing its FD.
     */
    void (*cancel)(struct knurl_watch *watch);
    /** Private library state; applications must not access it. */
    struct knurl_watch_impl *impl;
};

/** Library-owned timer receiver. Pass this handle as the first
 * argument to its methods. Obtain it through the documented constructor or
 * borrowed accessor. Do not copy, initialize or modify the shell or impl.
 */
struct knurl_timer {
    /** Cancel an owner-loop timer; userdata remains alive until retirement. */
    void (*cancel)(struct knurl_timer *timer);
    /** Private library state; applications must not access it. */
    struct knurl_timer_impl *impl;
};

struct knurl_error;
/** Library-owned waker receiver. Pass this handle as the first
 * argument to its methods. Obtain it through the documented constructor or
 * borrowed accessor. Do not copy, initialize or modify the shell or impl.
 */
struct knurl_waker {
    /** Signals coalesce through the loop's real wake descriptor. Stop/join all
     * producers before destroy; no pointer may be signalled after retirement.
     */
    knurl_status (*signal)(struct knurl_waker *waker,
                           struct knurl_error *error);
    /** Retire a waker on its owner loop after all signalling producers have
     * stopped.
     */
    knurl_status (*destroy)(struct knurl_waker *waker,
                            struct knurl_error *error);
    /** Private library state; applications must not access it. */
    struct knurl_waker_impl *impl;
};

#ifdef __cplusplus
}
#endif
#endif
