#ifndef ROTMOS_IO_H
#define ROTMOS_IO_H

/** @file io.h Public io API. Expected failures return rotmos_status and
 * optional bounded rotmos_error diagnostics. Take operations transfer
 * ownership only on success; views remain borrowed for their documented scope.
 */

#include <rotmos/common.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Produce a bounded chunk into caller storage. OK reports produced bytes; EOF
 * finishes and AGAIN pauses with no progress. Native reads use only retained
 * userdata/chunk storage; see rotmos_source_execution.
 */
typedef rotmos_status (*rotmos_source_read_fn)(void *buffer, size_t capacity,
                                               size_t *produced, void *userdata,
                                               rotmos_error *error);
/** Reset an optional source cursor on the owner loop; failure is terminal for
 * the requesting consumer.
 */
typedef rotmos_status (*rotmos_source_reset_fn)(void *userdata,
                                                rotmos_error *error);
/** Exactly-once owner-loop source cleanup: OK on complete consumption, failure
 * cause otherwise, CANCELLED on replacement. Native read acknowledgement
 * precedes close; retain userdata until then. Never reenter/longjmp.
 */
typedef void (*rotmos_source_close_fn)(void *userdata, rotmos_status outcome);
/** Execution of bounded source reads. OWNER_LOOP callbacks must return
 * promptly; NATIVE_READER uses retained userdata and acknowledges before
 * owner-loop close. Reset and close stay on the owner loop.
 */

typedef enum rotmos_source_execution {
    /** Nonblocking read/reset/close callbacks run on the owner loop. */
    ROTMOS_SOURCE_OWNER_LOOP = 0,
    /** Bounded reads run natively; acknowledgement precedes owner-loop close.
     */
    ROTMOS_SOURCE_NATIVE_READER
} rotmos_source_execution;

/** Configuration for source. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct rotmos_source_config {
    /** Bounded read callback; honor the enclosing offset/prefix/AGAIN contract.
     */
    rotmos_source_read_fn read;
    /** Optional cursor reset callback; never implies full-content
     * materialization.
     */
    rotmos_source_reset_fn reset;
    /** Exactly-once owner-loop source cleanup: OK on complete consumption,
     * failure cause otherwise, CANCELLED on replacement. Native read
     * acknowledgement precedes close; retain userdata until then. Never
     * reenter/longjmp.
     */
    rotmos_source_close_fn close;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
    /** Owner-loop nonblocking or retained native-reader execution. */
    rotmos_source_execution execution;
    /** Exact number of bytes; excludes any incidental trailing NUL. */
    rotmos_u64 length;
    /** Nonzero when length is declared; distinguishes unknown length from an
     * empty source.
     */
    int has_length;
    /** A selected live reply resets once on the owner loop after hooks and
     * before headers, when
     * reset is supplied. Suppressed output (HEAD/1xx/204/205/304) does not
     * reset/read. Other source consumers retain their explicit reset policy. */
    int reset_before_reply;
} rotmos_source_config;

/** Initialize source config with its documented defaults; call before applying
 * overrides.
 */
ROTMOS_API void rotmos_source_config_init(rotmos_source_config *config);
/** Construction does not call callbacks. Success owns one close notification.
 */
ROTMOS_API rotmos_status rotmos_source_new(const rotmos_source_config *config,
                                           rotmos_source **out,
                                           rotmos_error *error);
/** A copied in-memory source; construction never borrows the input storage. */
ROTMOS_API rotmos_status rotmos_source_bytes(rotmos_bytes bytes,
                                             rotmos_source **out,
                                             rotmos_error *error);
/** Opens and pins a regular file at its current length. Reads/reset use an
 * independent cursor; close releases the FD and never unlinks the path.
 * Later growth is excluded; truncation before the declared end fails. */
ROTMOS_API rotmos_status rotmos_source_file(const char *path,
                                            rotmos_source **out,
                                            rotmos_error *error);
/** Manual access is available before ownership passes to a reply/runtime.
 * A native upload handler may also read its borrowed source as documented in
 * upload.h; that view rejects reset, destroy and ownership transfer.
 */
ROTMOS_API rotmos_status rotmos_source_read(rotmos_source *source, void *buffer,
                                            size_t capacity, size_t *produced,
                                            rotmos_error *error);
/** Invoke the optional reset callback on an untransferred source; a borrowed
 * native-upload reader cannot reset.
 */
ROTMOS_API rotmos_status rotmos_source_reset(rotmos_source *source,
                                             rotmos_error *error);
/** Owner-loop readiness notification after runtime transfer; can be called
 * inside an owner-loop read callback and coalesces a later pull. A native
 * provider posts through a public waker. No response/request handle needed. */
ROTMOS_API rotmos_status rotmos_source_resume(rotmos_source *, rotmos_error *);
/** Cooperative query during a retained native read callback. Includes response
 * revocation and worker stopping. Close waits for read acknowledgement and
 * runs once on the owner loop; no HTTP callback joins a native reader. */
ROTMOS_API int rotmos_source_cancelled(const rotmos_source *);
/** Close an untransferred source once and free it; runtime-owned sources reject
 * destruction.
 */
ROTMOS_API rotmos_status rotmos_source_destroy(rotmos_source *source,
                                               rotmos_error *error);

#ifdef __cplusplus
}
#endif
#endif
