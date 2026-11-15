#ifndef KNURL_REPLY_H
#define KNURL_REPLY_H
/** @file reply.h Public reply API. Expected failures return knurl_status and
 * optional bounded knurl_error diagnostics. Take operations transfer
 * ownership only on success; views remain borrowed for their documented scope.
 * Content-Type arguments/headers require RFC 9110 media-type syntax, including
 * complete token/quoted parameters. Invalid values fail before replacement.
 */

#include <knurl/io.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Values for reply kind. */
typedef enum knurl_reply_kind {
    /** Staged payload representation: empty. */
    KNURL_REPLY_EMPTY = 0,
    /** Public bound for reply bytes, in bytes or entries as named. */
    KNURL_REPLY_BYTES,
    /** Staged payload representation: file. */
    KNURL_REPLY_FILE,
    /** Staged payload representation: source. */
    KNURL_REPLY_SOURCE
} knurl_reply_kind;

/** Configuration for source materialize. Use the owning operation's initializer
 * before applying overrides.
 */
typedef struct knurl_source_materialize_config {
    /** Maximum bytes for this operation; zero-inheritance/unlimited behavior is
     * operation-specific.
     */
    size_t max_bytes;
    /** Bounded transport/generation chunk size in bytes. */
    size_t chunk_bytes;
    /** Nonzero requests a source reset before explicit synchronous
     * materialization.
     */
    int reset_before;
    /** Nonzero requests a source reset after explicit synchronous
     * materialization.
     */
    int reset_after;
} knurl_source_materialize_config;

/** Standalone builders can be used by custom dispatchers and tests. */
KNURL_API knurl_status knurl_reply_new(knurl_reply **out, knurl_error *error);
/** Release a standalone builder and its selected payload; transferred runtime
 * builders cannot be destroyed.
 */
KNURL_API knurl_status knurl_reply_destroy(knurl_reply *reply,
                                           knurl_error *error);
/** Status-only selection releases payload/type while retaining headers. */
KNURL_API knurl_status knurl_reply_status(knurl_reply *reply, int status,
                                          knurl_error *error);
/** Append a copied header, preserving duplicates and registration order. */
KNURL_API knurl_status knurl_reply_header_add(knurl_reply *reply,
                                              const char *name,
                                              const char *value,
                                              knurl_error *error);
/** Replaces all case-insensitive occurrences at their first position. */
KNURL_API knurl_status knurl_reply_header_set(knurl_reply *reply,
                                              const char *name,
                                              const char *value,
                                              knurl_error *error);
/** Copy NUL-terminated text and content type into a staged reply; no network
 * output occurs.
 */
KNURL_API knurl_status knurl_reply_text(knurl_reply *reply, int status,
                                        const char *content_type,
                                        const char *text, knurl_error *error);
/** Copy binary bytes and content type into a staged reply; no network output
 * occurs.
 */
KNURL_API knurl_status knurl_reply_bytes(knurl_reply *reply, int status,
                                         const char *content_type,
                                         knurl_bytes body, knurl_error *error);
/** Copies a path. After-hooks may replace it or change the file before the
 * selected reply is submitted. Submission opens/pins the regular file; a
 * missing/unreadable path produces an empty 404. Bodyless statuses do not open
 * it; HEAD still resolves its representation. Explicit source_file pins early.
 */
KNURL_API knurl_status knurl_reply_file(knurl_reply *reply, int status,
                                        const char *content_type,
                                        const char *path, knurl_error *error);
/** No read/reset occurs. Success transfers the source until terminal cleanup.
 */
KNURL_API knurl_status knurl_reply_source_take(knurl_reply *reply, int status,
                                               const char *content_type,
                                               knurl_source *source,
                                               knurl_error *error);
/** Initialize source materialize config with its documented defaults; call
 * before applying overrides.
 */
KNURL_API void
knurl_source_materialize_config_init(knurl_source_materialize_config *config);
/** Synchronous, explicitly buffered operation. Borrows the source, never closes
 * it, and rejects AGAIN. Reset is performed only when explicitly requested.
 * Zero bounds select defaults (10 MiB / 8192 bytes). Failure preserves the
 * prior reply but may consume input unless reset_after is enabled. */
KNURL_API knurl_status knurl_reply_source_materialized(
    knurl_reply *reply, int status, const char *content_type,
    knurl_source *source, const knurl_source_materialize_config *config,
    knurl_error *error);

/** Synchronous file-backed materialization. Borrows an unowned source, resets
 * once when supported, and never closes it. Consumes to EOF in bounded chunks;
 * AGAIN fails. No memory/body ceiling is added. The target file-offset range
 * and filesystem limits still apply. NULL/empty directory selects /tmp.
 * Success pins a private unlinked regular file until delivery/replacement;
 * failure closes it and preserves the prior reply (input may be consumed).
 * This operation can block on its source and filesystem. */
KNURL_API knurl_status knurl_reply_source_spooled(
    knurl_reply *reply, int status, const char *content_type,
    knurl_source *source, const char *directory, knurl_error *error);

/** These snapshots borrow the builder's storage until its next mutation. */
KNURL_API int knurl_reply_is_selected(const knurl_reply *reply);
/** Read the staged HTTP status without submitting it. */
KNURL_API int knurl_reply_status_code(const knurl_reply *reply);
/** Read the selected payload representation without consuming it. */
KNURL_API knurl_reply_kind knurl_reply_payload_kind(const knurl_reply *reply);
/** Borrow the staged content type until the builder's next mutation or
 * destruction.
 */
KNURL_API knurl_bytes knurl_reply_content_type(const knurl_reply *reply);
/** Borrow a staged byte payload until mutation/destruction; does not read
 * file/source payloads.
 */
KNURL_API knurl_bytes knurl_reply_body(const knurl_reply *reply);
/** Return the ordered staged header count, including repeated fields. */
KNURL_API size_t knurl_reply_header_count(const knurl_reply *reply);
/** Borrow an ordered header by zero-based index until the next
 * mutation/destruction.
 */
KNURL_API knurl_status knurl_reply_header_at(const knurl_reply *reply,
                                             size_t index, knurl_header *out,
                                             knurl_error *error);

struct knurl_bytes;
struct knurl_error;
struct knurl_header;
struct knurl_json_source_config;
struct knurl_rest_reply_config;
struct knurl_source;
struct knurl_source_materialize_config;
struct knurl_xml_config;
struct knurl_xml_source_config;
struct lonejson_json_value;
struct lonejson_record_view;
struct lonejson_schema_view;
/** Library-owned reply receiver. Pass this handle as the first
 * argument to its methods. Obtain it through the documented constructor or
 * borrowed accessor. Do not copy, initialize or modify the shell or impl.
 */
struct knurl_reply {
    /** Stage an explicit live XML source; record storage remains borrowed until
     * terminal source release.
     */
    knurl_status (*xml)(struct knurl_reply *, int,
                        const struct knurl_xml_source_config *,
                        struct knurl_error *);
    /** Serialize XML synchronously into a copied staged reply; no record/schema
     * lifetime extends beyond return.
     */
    knurl_status (*xml_materialized)(struct knurl_reply *, int,
                                     const struct lonejson_record_view *,
                                     const struct knurl_xml_config *, size_t,
                                     struct knurl_error *);
    /** Release a standalone builder and its selected payload; transferred
     * runtime builders cannot be destroyed.
     */
    knurl_status (*destroy)(struct knurl_reply *reply,
                            struct knurl_error *error);
    /** Status-only selection releases payload/type while retaining headers. */
    knurl_status (*status)(struct knurl_reply *reply, int status,
                           struct knurl_error *error);
    /** Append a copied header, preserving duplicates and registration order. */
    knurl_status (*header_add)(struct knurl_reply *reply, const char *name,
                               const char *value, struct knurl_error *error);
    /** Replaces all case-insensitive occurrences at their first position. */
    knurl_status (*header_set)(struct knurl_reply *reply, const char *name,
                               const char *value, struct knurl_error *error);
    /** Copy NUL-terminated text and content type into a staged reply; no
     * network output occurs.
     */
    knurl_status (*text)(struct knurl_reply *reply, int status,
                         const char *content_type, const char *text,
                         struct knurl_error *error);
    /** Copy binary bytes and content type into a staged reply; no network
     * output occurs.
     */
    knurl_status (*bytes)(struct knurl_reply *reply, int status,
                          const char *content_type, struct knurl_bytes body,
                          struct knurl_error *error);
    /** Copies a path. After-hooks may replace it or change the file before the
     * selected reply is submitted. Submission opens/pins the regular file; a
     * missing/unreadable path produces an empty 404. Bodyless statuses do not
     * open it; HEAD still resolves its representation. Explicit source_file
     * pins early.
     */
    knurl_status (*file)(struct knurl_reply *reply, int status,
                         const char *content_type, const char *path,
                         struct knurl_error *error);
    /** No read/reset occurs. Success transfers the source until terminal
     * cleanup.
     */
    knurl_status (*source_take)(struct knurl_reply *reply, int status,
                                const char *content_type,
                                struct knurl_source *source,
                                struct knurl_error *error);
    /** Synchronous, explicitly buffered operation. Borrows the source, never
     * closes it, and rejects AGAIN. Reset is performed only when explicitly
     * requested. Zero bounds select defaults (10 MiB / 8192 bytes). Failure
     * preserves the prior reply but may consume input unless reset_after is
     * enabled. */
    knurl_status (*source_materialized)(
        struct knurl_reply *reply, int status, const char *content_type,
        struct knurl_source *source,
        const struct knurl_source_materialize_config *config,
        struct knurl_error *error);
    /** Synchronous file-backed materialization. Borrows an unowned source,
     * resets once when supported, and never closes it. Consumes to EOF in
     * bounded chunks; AGAIN fails. No memory/body ceiling is added. The target
     * file-offset range and filesystem limits still apply. NULL/empty directory
     * selects /tmp. Success pins a private unlinked regular file until
     * delivery/replacement; failure closes it and preserves the prior reply
     * (input may be consumed). This operation can block on its source and
     * filesystem. */
    knurl_status (*source_spooled)(struct knurl_reply *reply, int status,
                                   const char *content_type,
                                   struct knurl_source *source,
                                   const char *directory,
                                   struct knurl_error *error);
    /** These snapshots borrow the builder's storage until its next mutation. */
    int (*is_selected)(const struct knurl_reply *reply);
    /** Read the staged HTTP status without submitting it. */
    int (*status_code)(const struct knurl_reply *reply);
    /** Read the selected payload representation without consuming it. */
    knurl_reply_kind (*payload_kind)(const struct knurl_reply *reply);
    /** Borrow the staged content type until the builder's next mutation or
     * destruction.
     */
    struct knurl_bytes (*content_type)(const struct knurl_reply *reply);
    /** Borrow a staged byte payload until mutation/destruction; does not read
     * file/source payloads.
     */
    struct knurl_bytes (*body)(const struct knurl_reply *reply);
    /** Return the ordered staged header count, including repeated fields. */
    size_t (*header_count)(const struct knurl_reply *reply);
    /** Borrow an ordered header by zero-based index until the next
     * mutation/destruction.
     */
    knurl_status (*header_at)(const struct knurl_reply *reply, size_t index,
                              struct knurl_header *out,
                              struct knurl_error *error);
    /** Select an explicit live JSON generator; retain record/schema until
     * release. See knurl_source_json.
     */
    knurl_status (*json)(struct knurl_reply *reply, int status,
                         const struct knurl_json_source_config *config,
                         struct knurl_error *error);
    /** Serialize synchronously into a copied staged reply; zero max_bytes adds
     * no ceiling. Stack records may expire on return.
     */
    knurl_status (*json_materialized)(struct knurl_reply *reply, int status,
                                      const struct lonejson_schema_view *schema,
                                      const void *record, size_t max_bytes,
                                      struct knurl_error *error);
    /** Synchronously serializes into a private file, without a full JSON buffer
     * or retaining the record/schema after return. Directory/ownership follow
     * knurl_reply_source_spooled. Failure preserves the existing reply. */
    knurl_status (*json_spooled)(struct knurl_reply *, int status,
                                 const struct lonejson_schema_view *,
                                 const void *record, const char *directory,
                                 struct knurl_error *);
    /** Synchronous bounded JSON composition; copies before returning. Defaults:
     * 200, application/json; charset=utf-8, no output ceiling and a trailing
     * newline. Failure preserves the existing staged reply. Header composition
     * uses reply helpers.
     */
    knurl_status (*rest_json)(struct knurl_reply *,
                              const struct lonejson_json_value *,
                              const struct knurl_rest_reply_config *,
                              struct knurl_error *);
    /** Generic default envelope; callers can instead own their error policy
     * through on_error. Zero status derives 400/409/501/504/500 from the cause.
     */
    knurl_status (*error_json)(struct knurl_reply *, int status,
                               const char *kind,
                               const struct knurl_error *cause,
                               struct knurl_error *);
    /** Private library state; applications must not access it. */
    struct knurl_reply_impl *impl;
};

#ifdef __cplusplus
}
#endif
#endif
