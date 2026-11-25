#ifndef KNURL_AUTH_JSON_H
#define KNURL_AUTH_JSON_H

#include "auth_internal.h"

struct auth_record {
    char subject[129];
    char email[255];
    char secret[123];
    char seed[123];
    char revision[33];
    lonejson_int64 expires;
    lonejson_int64 updated;
    lonejson_int64 last_step;
    lonejson_int64 attempts;
    int disabled;
};

int knurl_auth_json_lock(knurl_auth_json *, knurl_error *);
knurl_status knurl_auth_json_read(knurl_auth_json *, const char *,
                                  struct auth_record *, knurl_error *);
knurl_status knurl_auth_json_write(knurl_auth_json *, const char *,
                                   const struct auth_record *, knurl_error *);
knurl_status knurl_auth_json_delete(knurl_auth_json *, const char *,
                                    knurl_error *);

#endif
