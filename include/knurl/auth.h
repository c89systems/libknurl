#ifndef KNURL_AUTH_H
#define KNURL_AUTH_H

#include <knurl/common.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct knurl_auth knurl_auth;
typedef struct knurl_auth_json knurl_auth_json;

#define KNURL_AUTH_TOTP_URI_CAPACITY 512

typedef enum knurl_auth_method {
    KNURL_AUTH_SESSION = 1,
    KNURL_AUTH_MACHINE
} knurl_auth_method;

typedef enum knurl_auth_login {
    KNURL_AUTH_LOGIN_PASSWORD = 0,
    KNURL_AUTH_LOGIN_TOTP,
    KNURL_AUTH_LOGIN_EMAIL
} knurl_auth_login;

/** Verified, stable subject. Borrowed from a request or a native snapshot. */
typedef struct knurl_auth_identity {
    knurl_auth_method method;
    char subject[129];
} knurl_auth_identity;

typedef enum knurl_auth_operation {
    KNURL_AUTH_ATTEMPT,
    KNURL_AUTH_PASSWORD,
    KNURL_AUTH_TOTP_SESSION,
    KNURL_AUTH_EMAIL,
    KNURL_AUTH_EMAIL_SAVE,
    KNURL_AUTH_EMAIL_SESSION,
    KNURL_AUTH_SESSION_SAVE,
    KNURL_AUTH_SESSION_FIND,
    KNURL_AUTH_SESSION_REVOKE,
    KNURL_AUTH_MACHINE_VERIFY,
    KNURL_AUTH_TOTP_ENROLL
} knurl_auth_operation;

/** On PASSWORD, EMAIL, SESSION_FIND and MACHINE_VERIFY, return a canonical
 * subject in identity. EMAIL also returns the enrolled delivery address.
 * ATTEMPT limits login attempts per peer and scope. TOTP_SESSION and
 * EMAIL_SESSION verify the factor, consume it and create the session in one
 * transaction. TOTP_SESSION returns CONFLICT when a verified account needs
 * enrollment. EMAIL_SAVE replaces the challenge subject to a resend limit.
 * TOTP_ENROLL receives a subject from a verified session. An empty secret
 * requests first enrollment, a code authorizes rotation. Verify the supplied
 * session digest and revision atomically with the update. Return the
 * provisioning URI on success.
 * Session operations use an opaque digest scoped to the policy and host.
 * Strings are borrowed for the call. Zero the unused output fields.
 */
typedef struct knurl_auth_exchange {
    /** Stable policy name, used to isolate machine credentials. */
    const char *policy;
    /** Opaque namespace for this policy and downstream authority. */
    const char *scope;
    /** Login name for ATTEMPT, PASSWORD and EMAIL. Canonical subject for
     * TOTP, email challenge state and SESSION_SAVE. */
    const char *subject;
    /** Password, six-digit TOTP code, or the raw machine Authorization field,
     * supplied only to the corresponding verification operation. */
    const char *secret;
    /** Lowercase hex digest for session and email challenge state operations.
     */
    const char *digest;
    /** New session digest for factor/session operations. Existing session
     * digest for enrollment. */
    const char *session_digest;
    knurl_auth_login login;
    /** Effective client address after the server's trusted forwarding policy.
     */
    const char *peer;
    /** Expiry interval from the time of EMAIL_SAVE or SESSION_SAVE. */
    unsigned long lifetime_seconds;
    knurl_auth_identity identity;
    char email[255];
    /** Optional opaque credential revision, propagated to the state backend
     * when both operations use the same provider. */
    char revision[33];
    char totp_uri[KNURL_AUTH_TOTP_URI_CAPACITY];
} knurl_auth_exchange;

/** Runs on a native worker. Return EOF for a denial or missing entry,
 * LIMIT for throttling, and another terminal failure for backend errors.
 * State operations must be atomic across processes/threads. A provider may
 * implement only the operations used by the configured policies. */
typedef knurl_status (*knurl_auth_backend_fn)(knurl_auth_operation,
                                              knurl_auth_exchange *, void *,
                                              knurl_error *);
/** Deliver the six-digit code to the enrolled address on an auth worker. */
typedef knurl_status (*knurl_auth_email_fn)(const char *, const char *, void *,
                                            knurl_error *);
/** Optional account revalidation after session lookup, on a native worker.
 * Return OK to admit, EOF to deny, or another terminal status when the
 * identity provider is unavailable. */
typedef knurl_status (*knurl_auth_session_check_fn)(const knurl_auth_identity *,
                                                    void *, knurl_error *);

typedef struct knurl_auth_smtp {
    const char *url;
    const char *from;
    const char *username;
    const char *password;
    const char *ca_file;
    long timeout_ms;
} knurl_auth_smtp;

/** JWT bearer validation. By default, discover the issuer's JWKS URI and
 * refresh signing keys as needed. jwks_json can seed the cache when jwks_uri
 * is explicit. Set pin_jwks to use only those configured keys. */
typedef struct knurl_auth_oidc {
    const char *issuer;
    const char *audience;
    const char *jwks_uri;
    const char *jwks_json;
    const char *scope;
    /** Default is at+jwt. Set to the issuer's access-token JOSE type. */
    const char *token_type;
    const char *ca_file;
    int pin_jwks;
} knurl_auth_oidc;

typedef struct knurl_oauth2_client knurl_oauth2_client;
/** OAuth 2.0 client-credentials grant. Configure an HTTPS issuer for discovery
 * or an explicit HTTPS token_endpoint. Defaults to client_secret_basic.
 * The process-local cache acquires and renews the bearer token on demand. */
typedef struct knurl_oauth2_config {
    const char *issuer;
    const char *token_endpoint;
    const char *client_id;
    const char *client_secret;
    const char *scope;
    const char *audience;
    const char *resource;
    const char *ca_file;
    int client_secret_post;
} knurl_oauth2_config;

KNURL_API knurl_status knurl_oauth2_client_new(const knurl_oauth2_config *,
                                               knurl_oauth2_client **,
                                               knurl_error *);
KNURL_API void knurl_oauth2_client_close(knurl_oauth2_client *);
/** Return a copied access token, with automatic renewal. This can block on
 * HTTPS I/O. Call from an application worker, never from the event loop. */
KNURL_API knurl_status knurl_oauth2_client_token(knurl_oauth2_client *, char *,
                                                 size_t, knurl_error *);
/** Obtain and attach an outbound bearer token on a native worker. All route
 * targets, including alternatives, must use HTTPS. The route retains client. */
KNURL_API knurl_status knurl_route_set_proxy_oauth2(knurl_route *,
                                                    knurl_oauth2_client *,
                                                    knurl_error *);

typedef struct knurl_auth_config {
    /** Required policy identifier. Distinct browser policies use distinct
     * names so a session issued under one cannot satisfy another. */
    const char *name;
    knurl_auth_method method;
    knurl_auth_login login;
    knurl_auth_backend_fn backend;
    void *userdata;
    /** Optional separate backend for PASSWORD and EMAIL. */
    knurl_auth_backend_fn users;
    void *users_userdata;
    /** Optional retained JSON store for the default backend. */
    knurl_auth_json *json;
    knurl_auth_email_fn send_email;
    void *email_userdata;
    knurl_auth_smtp smtp;
    unsigned long session_seconds;
    /** Hex-encoded 32-byte secret for email challenge digests. A JSON store
     * supplies its own key when this is omitted. */
    const char *email_key;
    /** Machine-only alternatives to a backend. Configure a Basic credential
     * pair or JWT bearer validation. Use a random client secret. */
    const char *client_id;
    const char *client_secret;
    knurl_auth_oidc oidc;
    knurl_auth_session_check_fn session_check;
    void *session_check_userdata;
} knurl_auth_config;

KNURL_API void knurl_auth_config_init(knurl_auth_config *);
/** Copies configuration strings. Custom callback userdata stays borrowed
 * through all attached routes and in-flight tasks. */
KNURL_API knurl_status knurl_auth_new(const knurl_auth_config *, knurl_auth **,
                                      knurl_error *);
KNURL_API void knurl_auth_close(knurl_auth *);
/** Attach or clear a policy on an inactive route or group. A route's explicit
 * policy takes precedence over its group's default. */
KNURL_API knurl_status knurl_route_set_auth(knurl_route *, knurl_auth *,
                                            knurl_error *);
KNURL_API knurl_status knurl_route_group_set_auth(knurl_route_group *,
                                                  knurl_auth *, knurl_error *);
KNURL_API const knurl_auth_identity *knurl_request_auth(const knurl_request *);

/** Register a browser endpoint under its exact path. POST bodies use
 * application/x-www-form-urlencoded. TLS is required for all auth routes. */
typedef enum knurl_auth_endpoint {
    KNURL_AUTH_LOGIN,
    KNURL_AUTH_EMAIL_START,
    KNURL_AUTH_LOGOUT,
    /** Authorize with this policy's session cookie. POST an empty form to
     * enroll, or code=<current TOTP> to rotate. */
    KNURL_AUTH_TOTP_ENROLL_ENDPOINT
} knurl_auth_endpoint;
KNURL_API knurl_status knurl_auth_route(knurl_router *, const char *,
                                        knurl_auth *, knurl_auth_endpoint,
                                        knurl_route **, knurl_error *);

/** Open an existing private state directory. Initialize explicitly via
 * knurl_auth_json_init before opening. The directory contains the local key
 * and a fixed set of atomically replaced JSON files. Use a local filesystem
 * with flock. */
KNURL_API knurl_status knurl_auth_json_init(const char *, knurl_error *);
KNURL_API knurl_status knurl_auth_json_open(const char *, knurl_auth_json **,
                                            knurl_error *);
KNURL_API void knurl_auth_json_close(knurl_auth_json *);
typedef enum knurl_auth_admin {
    KNURL_AUTH_USER_PASSWORD,
    KNURL_AUTH_USER_EMAIL,
    KNURL_AUTH_USER_ENABLE,
    KNURL_AUTH_USER_DISABLE
} knurl_auth_admin;
/** Create/update a named account. Account changes revoke its sessions.
 * Use NULL for operations with no value.
 */
KNURL_API knurl_status knurl_auth_json_admin(knurl_auth_json *,
                                             knurl_auth_admin, const char *,
                                             const char *, knurl_error *);
/** Issue or rotate a machine credential within the named policy. */
KNURL_API knurl_status knurl_auth_json_issue(knurl_auth_json *, const char *,
                                             const char *, char[65],
                                             knurl_error *);
KNURL_API knurl_status knurl_auth_json_revoke(knurl_auth_json *, const char *,
                                              const char *, knurl_error *);
/** Generate and store an encrypted TOTP enrollment and return its URI once.
 * Nonzero rotate permits replacing an existing enrollment. */
KNURL_API knurl_status
knurl_auth_json_enroll_totp(knurl_auth_json *, const char *, int,
                            char[KNURL_AUTH_TOTP_URI_CAPACITY], knurl_error *);
/** Remove expired sessions, challenges and throttle records. */
KNURL_API knurl_status knurl_auth_json_prune(knurl_auth_json *, knurl_error *);

#ifdef __cplusplus
}
#endif
#endif
