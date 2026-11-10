#ifndef KNURL_IO_H
#define KNURL_IO_H

/** @file io.h Public io API. Expected failures return knurl_status and
 * optional bounded knurl_error diagnostics. Take operations transfer
 * ownership only on success; views remain borrowed for their documented scope.
 */

#include <knurl/common.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Produce a bounded chunk into caller storage. OK reports produced bytes; EOF
 * finishes and AGAIN pauses with no progress. Native reads use only retained
 * userdata/chunk storage; see knurl_source_execution.
 */
typedef knurl_status (*knurl_source_read_fn)(void *buffer, size_t capacity,
                                             size_t *produced, void *userdata,
                                             knurl_error *error);
/** Reset an optional source cursor on the owner loop; failure is terminal for
 * the requesting consumer.
 */
typedef knurl_status (*knurl_source_reset_fn)(void *userdata,
                                              knurl_error *error);
/** Exactly-once owner-loop source cleanup: OK on complete consumption, failure
 * cause otherwise, CANCELLED on replacement. Native read acknowledgement
 * precedes close; retain userdata until then. Never reenter/longjmp.
 */
typedef void (*knurl_source_close_fn)(void *userdata, knurl_status outcome);
/** Execution of bounded source reads. OWNER_LOOP callbacks must return
 * promptly; NATIVE_READER uses retained userdata and acknowledges before
 * owner-loop close. Reset and close stay on the owner loop.
 */

typedef enum knurl_source_execution {
    /** Nonblocking read/reset/close callbacks run on the owner loop. */
    KNURL_SOURCE_OWNER_LOOP = 0,
    /** Bounded reads run natively; acknowledgement precedes owner-loop close.
     */
    KNURL_SOURCE_NATIVE_READER
} knurl_source_execution;

/** Configuration for source. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct knurl_source_config {
    /** Bounded read callback; honor the enclosing offset/prefix/AGAIN contract.
     */
    knurl_source_read_fn read;
    /** Optional cursor reset callback; never implies full-content
     * materialization.
     */
    knurl_source_reset_fn reset;
    /** Exactly-once owner-loop source cleanup: OK on complete consumption,
     * failure cause otherwise, CANCELLED on replacement. Native read
     * acknowledgement precedes close; retain userdata until then. Never
     * reenter/longjmp.
     */
    knurl_source_close_fn close;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
    /** Owner-loop nonblocking or retained native-reader execution. */
    knurl_source_execution execution;
    /** Declared source length in bytes; used only when has_length is nonzero.
     * A known empty source has length zero and has_length nonzero.
     */
    knurl_u64 length;
    /** Nonzero when length is declared; distinguishes unknown length from an
     * empty source.
     */
    int has_length;
    /** A selected live reply resets once on the owner loop after hooks and
     * before headers, when
     * reset is supplied. Suppressed output (HEAD/1xx/204/205/304) does not
     * reset/read. Other source consumers retain their explicit reset policy. */
    int reset_before_reply;
} knurl_source_config;

/** Initialize source config with its documented defaults; call before applying
 * overrides.
 */
KNURL_API void knurl_source_config_init(knurl_source_config *config);
/** Construction does not call callbacks. Success owns one close notification.
 */
KNURL_API knurl_status knurl_source_new(const knurl_source_config *config,
                                        knurl_source **out, knurl_error *error);
/** A copied in-memory source; construction never borrows the input storage. */
KNURL_API knurl_status knurl_source_bytes(knurl_bytes bytes, knurl_source **out,
                                          knurl_error *error);
/** Opens and pins a regular file at its current length. Reads/reset use an
 * independent cursor; close releases the FD and never unlinks the path.
 * Later growth is excluded; truncation before the declared end fails. */
KNURL_API knurl_status knurl_source_file(const char *path, knurl_source **out,
                                         knurl_error *error);
/** Manual access is available before ownership passes to a reply/runtime.
 * A native upload handler may also read its borrowed source as documented in
 * upload.h; that view rejects reset, destroy and ownership transfer.
 */
KNURL_API knurl_status knurl_source_read(knurl_source *source, void *buffer,
                                         size_t capacity, size_t *produced,
                                         knurl_error *error);
/** Invoke the optional reset callback on an untransferred source; a borrowed
 * native-upload reader cannot reset.
 */
KNURL_API knurl_status knurl_source_reset(knurl_source *source,
                                          knurl_error *error);
/** Owner-loop readiness notification after runtime transfer; can be called
 * inside an owner-loop read callback and coalesces a later pull. A native
 * provider posts through a public waker. No response/request handle needed. */
KNURL_API knurl_status knurl_source_resume(knurl_source *, knurl_error *);
/** Cooperative query during a retained native read callback. Includes response
 * revocation and worker stopping. Close waits for read acknowledgement and
 * runs once on the owner loop; no HTTP callback joins a native reader. */
KNURL_API int knurl_source_cancelled(const knurl_source *);
/** Close an untransferred source once and free it; runtime-owned sources reject
 * destruction.
 */
KNURL_API knurl_status knurl_source_destroy(knurl_source *source,
                                            knurl_error *error);

#ifdef __cplusplus
}
#endif
#endif
