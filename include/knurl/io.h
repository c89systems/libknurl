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

struct lonejson_record_view;
/** Borrowed mapped-array item callback; a non-OK result stops iteration. */
typedef knurl_status (*knurl_json_array_item_fn)(
    size_t index, const struct lonejson_record_view *item, void *userdata,
    knurl_error *error);
/** Accept a complete borrowed JSON chunk or return a terminal failure. */
typedef knurl_status (*knurl_json_sink_fn)(const void *, size_t, void *,
                                           knurl_error *);

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

struct knurl_error;
struct knurl_xml_config;
struct lonejson;
struct lonejson_array_rewrite_options;
struct lonejson_schema_view;
/** Library-owned source receiver. Pass this handle as the first
 * argument to its methods. Obtain it through the documented constructor or
 * borrowed accessor. Do not copy, initialize or modify the shell or impl.
 */
struct knurl_source {
    /** Synchronous mapped input. Borrows source; consumes to EOF, never closes
     * it. AGAIN fails; live blocking readers execute on native threads. Caller
     * supplies initialized storage; reset before parse and on failure,
     * preserving fixed array backing. Maps and optional runtime use the
     * released public LoneJSON schema interface. */
    knurl_status (*xml_parse)(struct knurl_source *,
                              const struct lonejson_schema_view *,
                              const struct knurl_xml_config *, void *,
                              struct knurl_error *);
    /** Synchronous mapped input from the current source position. Borrows
     * source and consumes through EOF; never resets or closes it. AGAIN fails
     * with STATE; live blocking readers belong on native threads. Caller
     * supplies initialized mutable record storage and owns cleanup, including
     * partial parse failure. */
    knurl_status (*json_parse)(struct knurl_source *,
                               const struct lonejson_schema_view *, void *,
                               struct knurl_error *);
    /** Synchronous selected-array iteration, matching Vectis. NULL/empty
     * selector selects the root array; otherwise it selects one direct object
     * key. Caller provides initialized, empty mapped storage. Each item is
     * reset before parsing and after delivery, including failures, preserving
     * fixed-capacity array backing. Record reuse is selected per stream for
     * both owned and supplied runtimes without changing their clearing policy.
     * Caller owns final cleanup. The callback borrows that same record at a
     * zero-based index; copy values that must survive return. No array/body is
     * collected. Non-OK callback results stop iteration. Sources are borrowed
     * and read from their current position; AGAIN fails with STATE (use a
     * blocking reader on a native thread for live input). Request bodies must
     * be completed and are rewound before and after, including failure. */
    knurl_status (*json_array_each)(struct knurl_source *, const char *selector,
                                    const struct lonejson_schema_view *,
                                    void *item, knurl_json_array_item_fn,
                                    void *userdata, struct knurl_error *);
    /** Vectis selected-array rewriting through LoneJSON's public mapped
     * callback options. Borrows source, optional runtime, options/item storage
     * and sink. Reads from the current position without reset/close. Output
     * flows before input EOF; no complete document/array is collected. */
    knurl_status (*json_array_rewrite)(
        struct knurl_source *, const char *, struct lonejson *,
        const struct lonejson_array_rewrite_options *, knurl_json_sink_fn,
        void *, struct knurl_error *);
    /** Manual access is available before ownership passes to a reply/runtime.
     * A native upload handler may also read its borrowed source as documented
     * in upload.h; that view rejects reset, destroy and ownership transfer.
     */
    knurl_status (*read)(struct knurl_source *source, void *buffer,
                         size_t capacity, size_t *produced,
                         struct knurl_error *error);
    /** Invoke the optional reset callback on an untransferred source; a
     * borrowed native-upload reader cannot reset.
     */
    knurl_status (*reset)(struct knurl_source *source,
                          struct knurl_error *error);
    /** Owner-loop readiness notification after runtime transfer; can be called
     * inside an owner-loop read callback and coalesces a later pull. A native
     * provider posts through a public waker. No response/request handle needed.
     */
    knurl_status (*resume)(struct knurl_source *, struct knurl_error *);
    /** Cooperative query during a retained native read callback. Includes
     * response revocation and worker stopping. Close waits for read
     * acknowledgement and runs once on the owner loop; no HTTP callback joins a
     * native reader. */
    int (*cancelled)(const struct knurl_source *);
    /** Close an untransferred source once and free it; runtime-owned sources
     * reject destruction.
     */
    knurl_status (*destroy)(struct knurl_source *source,
                            struct knurl_error *error);
    /** Private library state; applications must not access it. */
    struct knurl_source_impl *impl;
};

#ifdef __cplusplus
}
#endif
#endif
