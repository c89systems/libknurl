#ifndef KNURL_AUTH_INTERNAL_H
#define KNURL_AUTH_INTERNAL_H

#include <knurl/auth.h>
#include "request_internal.h"
#include "executor.h"
#include <lonejson.h>

#define KNURL_AUTH_COOKIE_PREFIX "__Host-knurl-"

struct knurl_auth {
    knurl_auth_config config;
    size_t references;
    unsigned char email_key[32];
    char client_digest[65];
    struct knurl_auth_issuer *issuer;
};

struct knurl_auth_json {
    size_t references;
    int directory;
    unsigned char key[32];
};

struct knurl_auth_job {
    knurl_auth *auth;
    knurl_request *request;        /* owner loop only */
    struct knurl_runtime *runtime; /* owner loop only */
    knurl_job *job;
    knurl_auth_endpoint endpoint;
    int admission;
    knurl_status status;
    char scope[512];
    char peer[46];
    char subject[129];
    char credential[16385];
    char code[17];
    char cookie[65];
    knurl_auth_identity identity;
};

void knurl_auth_retain(knurl_auth *);
void knurl_auth_json_retain(knurl_auth_json *);
knurl_status knurl_auth_json_exchange(knurl_auth_operation,
                                      knurl_auth_exchange *, void *,
                                      knurl_error *);
int knurl_auth_name(const char *);
int knurl_auth_email_address(const char *);
int knurl_auth_basic(const char *, char[129], const char **);
void knurl_auth_basic_clear(const char *);
int knurl_auth_random_hex(char *, size_t);
void knurl_auth_hex(const unsigned char *, size_t, char *);
int knurl_auth_unhex(const char *, unsigned char *, size_t);
int knurl_auth_password_make(const char *, char[97]);
int knurl_auth_password_check(const char *, const char *);
int knurl_auth_seed_seal(const unsigned char[32], const char *, const char *,
                         char[123]);
lonejson_int64 knurl_auth_totp_step(const unsigned char[32], const char *,
                                    const char *, const char *, lonejson_int64,
                                    lonejson_int64);
int knurl_auth_digest(const unsigned char *, const char *, const char *,
                      const char *, char[65]);
knurl_status knurl_auth_smtp_send(const char *, const char *, void *,
                                  knurl_error *);
knurl_status knurl_auth_admit(knurl_request *, knurl_request_plan *,
                              knurl_error *);
void knurl_auth_disconnect(knurl_request *);
void knurl_auth_job_free(struct knurl_auth_job *);
const char *knurl_auth_challenge(const knurl_auth *);
knurl_status knurl_auth_machine_verify(knurl_auth_exchange *, knurl_auth *,
                                       knurl_error *);
knurl_status knurl_auth_oidc_init(const knurl_auth_oidc *,
                                  struct knurl_auth_issuer **, knurl_error *);
void knurl_auth_oidc_close(struct knurl_auth_issuer *);
knurl_status knurl_auth_oidc_verify(struct knurl_auth_issuer *,
                                    knurl_auth_exchange *, knurl_error *);
knurl_status knurl_auth_start(struct knurl_auth_job *, knurl_error *);
knurl_status knurl_auth_reply(knurl_request *, int, const char *, const char *,
                              knurl_error *);
int knurl_auth_cookie(knurl_request *, const knurl_auth *, char[65]);
void knurl_kore_headers_resume(knurl_request *, int);
knurl_status knurl_router_headers_selected(knurl_request *,
                                           knurl_request_plan *, knurl_error *);

#endif
