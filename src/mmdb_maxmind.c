#define _POSIX_C_SOURCE 200809L
#include "internal.h"
#include <knurl/mmdb.h>
#include <maxminddb.h>
#include <stdlib.h>
#include <string.h>

static knurl_status failure(knurl_error *error, int code)
{
    knurl_status status =
        code == MMDB_OUT_OF_MEMORY_ERROR ? KNURL_NOMEM
        : (code == MMDB_FILE_OPEN_ERROR || code == MMDB_IO_ERROR)
            ? KNURL_IO
            : KNURL_BACKEND;
    knurl_fail(error, status, "mmdb.maxmind", MMDB_strerror(code));
    if (error != NULL) {
        error->source = KNURL_ERROR_BACKEND;
        error->native_code = code;
    }
    return status;
}
static void close_db(void *userdata)
{
    MMDB_s *db = (MMDB_s *)userdata;
    MMDB_close(db);
    free(db);
}
static knurl_status lookup(const knurl_mmdb_address *address,
                           knurl_mmdb_country *country, void *userdata,
                           knurl_error *error)
{
    static const char *const root_path[] = {NULL};
    static const char *const country_path[] = {"country", NULL};
    static const char *const code_path[] = {"country", "iso_code", NULL};
    MMDB_s *db = (MMDB_s *)userdata;
    struct sockaddr_in ipv4;
    struct sockaddr_in6 ipv6;
    const struct sockaddr *socket_address;
    MMDB_lookup_result_s result;
    MMDB_entry_data_s data;
    int code;
    if (address->size == 4) {
        memset(&ipv4, 0, sizeof(ipv4));
        ipv4.sin_family = AF_INET;
        memcpy(&ipv4.sin_addr, address->bytes, 4);
        socket_address = (const struct sockaddr *)&ipv4;
    } else {
        if (db->metadata.ip_version == 4)
            return KNURL_EOF;
        memset(&ipv6, 0, sizeof(ipv6));
        ipv6.sin6_family = AF_INET6;
        memcpy(&ipv6.sin6_addr, address->bytes, 16);
        socket_address = (const struct sockaddr *)&ipv6;
    }
    result = MMDB_lookup_sockaddr(db, socket_address, &code);
    if (code != MMDB_SUCCESS)
        return failure(error, code);
    if (!result.found_entry)
        return KNURL_EOF;
    code = MMDB_aget_value(&result.entry, &data, code_path);
    if (code == MMDB_LOOKUP_PATH_DOES_NOT_MATCH_DATA_ERROR) {
        /* A missing field and a scalar where a map was expected share the
         * upstream error code. Inspect shape only on this exceptional path;
         * successful lookups navigate the complete path once. */
        code = MMDB_aget_value(&result.entry, &data, root_path);
        if (code != MMDB_SUCCESS)
            return failure(error, code);
        if (data.type != MMDB_DATA_TYPE_MAP)
            return failure(error, MMDB_INVALID_DATA_ERROR);
        code = MMDB_aget_value(&result.entry, &data, country_path);
        if (code == MMDB_LOOKUP_PATH_DOES_NOT_MATCH_DATA_ERROR)
            return KNURL_EOF;
        if (code != MMDB_SUCCESS)
            return failure(error, code);
        if (data.type != MMDB_DATA_TYPE_MAP)
            return failure(error, MMDB_INVALID_DATA_ERROR);
        return KNURL_EOF;
    }
    if (code != MMDB_SUCCESS)
        return failure(error, code);
    if (!data.has_data)
        return KNURL_EOF;
    if (data.type != MMDB_DATA_TYPE_UTF8_STRING || data.data_size != 2)
        return failure(error, MMDB_INVALID_DATA_ERROR);
    memcpy(country->code, data.utf8_string, 2);
    country->code[2] = 0;
    return KNURL_OK;
}
knurl_status knurl_mmdb_maxmind_open(const char *path, knurl_mmdb **out,
                                     knurl_error *error)
{
    MMDB_s *db;
    knurl_mmdb_config config;
    knurl_status status;
    int code;
    if (out != NULL)
        *out = NULL;
    if (path == NULL || path[0] == 0 || out == NULL)
        return knurl_fail(error, KNURL_INVALID, "mmdb.open",
                          "path and output required");
    db = (MMDB_s *)calloc(1, sizeof(*db));
    if (db == NULL)
        return knurl_fail(error, KNURL_NOMEM, "mmdb.open", "allocation failed");
    code = MMDB_open(path, MMDB_MODE_MMAP, db);
    if (code != MMDB_SUCCESS) {
        free(db);
        return failure(error, code);
    }
    knurl_mmdb_config_init(&config);
    config.lookup = lookup;
    config.close = close_db;
    config.userdata = db;
    status = knurl_mmdb_new(&config, out, error);
    if (status != KNURL_OK)
        close_db(db);
    return status;
}
