#include "internal.h"
#include "mmdb_internal.h"
#include <knurl/mmdb.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>

void knurl_mmdb_config_init(knurl_mmdb_config *config)
{
    if (config != NULL)
        memset(config, 0, sizeof(*config));
}
knurl_status knurl_mmdb_new(const knurl_mmdb_config *config, knurl_mmdb **out,
                            knurl_error *error)
{
    struct knurl_mmdb_impl *impl;
    static const knurl_mmdb methods = {knurl_mmdb_lookup, knurl_mmdb_close,
                                       NULL};
    if (out != NULL)
        *out = NULL;
    if (config == NULL || config->lookup == NULL || out == NULL)
        return knurl_fail(error, KNURL_INVALID, "mmdb.new",
                          "lookup and output required");
    impl = (struct knurl_mmdb_impl *)calloc(1, sizeof(*impl));
    if (impl == NULL)
        return knurl_fail(error, KNURL_NOMEM, "mmdb.new", "allocation failed");
    impl->receiver = methods;
    impl->receiver.impl = impl;
    impl->config = *config;
    *out = &impl->receiver;
    knurl_error_clear(error);
    return KNURL_OK;
}
knurl_status knurl_mmdb_address_parse(const char *text, knurl_mmdb_address *out,
                                      knurl_error *error)
{
    if (out != NULL)
        memset(out, 0, sizeof(*out));
    /* Darwin's inet_pton accepts %zone suffixes and embeds interface scope.
     * This API accepts unscoped numeric addresses on every platform. */
    if (text != NULL && out != NULL && strchr(text, '%') == NULL) {
        if (inet_pton(AF_INET, text, out->bytes) == 1)
            out->size = 4;
        else if (inet_pton(AF_INET6, text, out->bytes) == 1)
            out->size = 16;
        if (out->size != 0) {
            knurl_error_clear(error);
            return KNURL_OK;
        }
        memset(out, 0, sizeof(*out));
    }
    return knurl_fail(error, KNURL_INVALID, "mmdb.address",
                      "numeric IPv4 or IPv6 required");
}
knurl_status knurl_mmdb_lookup(knurl_mmdb *db,
                               const knurl_mmdb_address *address,
                               knurl_mmdb_country *out, knurl_error *error)
{
    static const unsigned char mapped[12] = {0, 0, 0, 0, 0,   0,
                                             0, 0, 0, 0, 255, 255};
    knurl_mmdb_address normalized;
    knurl_mmdb_country result;
    knurl_status status;
    if (out != NULL)
        memset(out, 0, sizeof(*out));
    knurl_error_clear(error);
    if (db == NULL || address == NULL || out == NULL ||
        (address->size != 4 && address->size != 16))
        return knurl_fail(error, KNURL_INVALID, "mmdb.lookup",
                          "provider, address and output required");
    normalized = *address;
    if (address->size == 16 && memcmp(address->bytes, mapped, 12) == 0) {
        memmove(normalized.bytes, address->bytes + 12, 4);
        normalized.size = 4;
    }
    memset(&result, 0, sizeof(result));
    status = db->impl->config.lookup(&normalized, &result,
                                     db->impl->config.userdata, error);
    if (status == KNURL_OK) {
        if (result.code[0] < 'A' || result.code[0] > 'Z' ||
            result.code[1] < 'A' || result.code[1] > 'Z' || result.code[2] != 0)
            return knurl_fail(error, KNURL_BACKEND, "mmdb.lookup",
                              "invalid country code");
        *out = result;
    }
    if (status == KNURL_OK || status == KNURL_EOF)
        knurl_error_clear(error);
    return status;
}
knurl_status knurl_mmdb_close(knurl_mmdb *db, knurl_error *error)
{
    if (db != NULL) {
        if (db->impl->config.close != NULL)
            db->impl->config.close(db->impl->config.userdata);
        free(db->impl);
    }
    knurl_error_clear(error);
    return KNURL_OK;
}
