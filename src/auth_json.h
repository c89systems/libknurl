#ifndef KNURL_AUTH_JSON_H
#define KNURL_AUTH_JSON_H

#include "auth_internal.h"

struct auth_record {
    char key[67];
    char subject[129];
    char policy[129];
    char email[255];
    char secret[123];
    char seed[123];
    char revision[33];
    char enrollment[33];
    lonejson_int64 expires;
    lonejson_int64 updated;
    lonejson_int64 last_step;
    lonejson_int64 attempts;
    int disabled;
};

enum auth_collection {
    AUTH_ACCOUNTS,
    AUTH_SESSIONS,
    AUTH_EMAIL_CODES,
    AUTH_TOTP_STEPS,
    AUTH_ATTEMPTS,
    AUTH_COLLECTIONS
};

struct auth_document {
    lonejson_object_array records;
    int loaded;
    int dirty;
};

struct auth_transaction {
    knurl_auth_json *store;
    lonejson *runtime;
    struct auth_document documents[AUTH_COLLECTIONS];
    int lock;
};

knurl_status knurl_auth_json_begin(knurl_auth_json *, int,
                                   struct auth_transaction *, knurl_error *);
knurl_status knurl_auth_json_end(struct auth_transaction *, knurl_status,
                                 knurl_error *);
knurl_status knurl_auth_json_read(struct auth_transaction *, const char *,
                                  struct auth_record *, knurl_error *);
knurl_status knurl_auth_json_write(struct auth_transaction *, const char *,
                                   const struct auth_record *, knurl_error *);
knurl_status knurl_auth_json_delete(struct auth_transaction *, const char *,
                                    knurl_error *);

#endif
