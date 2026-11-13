#define _POSIX_C_SOURCE 200809L
#include <knurl/knurl.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static knurl_status open_db(const char *path, knurl_mmdb **out,
                            knurl_error *error)
{
    return knurl_mmdb_open(path, NULL, out, error);
}
static const char *root;
static void open_fixture(const char *name, knurl_mmdb **out)
{
    char path[1024];
    knurl_error error;
    assert(strlen(root) + strlen(name) + 7 < sizeof(path));
    assert(snprintf(path, sizeof(path), "%s/%s.mmdb", root, name) <
           (int)sizeof(path));
    if (open_db(path, out, &error) != KNURL_OK) {
        fprintf(stderr, "%s: %s\n", path, error.message);
        abort();
    }
}
static void expect(knurl_mmdb *db, const char *ip, const char *code,
                   knurl_status expected)
{
    knurl_mmdb_address address;
    knurl_mmdb_country country;
    knurl_error error;
    knurl_status status;
    assert(knurl_mmdb_address_parse(ip, &address, &error) == KNURL_OK);
    memset(&country, 'x', sizeof(country));
    status = db->lookup(db, &address, &country, &error);
    if (status != expected) {
        fprintf(stderr, "lookup %s: %d expected %d (%s)\n", ip, status,
                expected, error.message);
        abort();
    }
    assert(strcmp(country.code, code) == 0);
}
static knurl_status custom(const knurl_mmdb_address *address,
                           knurl_mmdb_country *out, void *userdata,
                           knurl_error *error)
{
    (void)userdata;
    (void)error;
    assert(address->size == 4);
    memcpy(out->code, address->bytes[0] ? "SE" : "xx", 3);
    return KNURL_OK;
}
static void closed(void *userdata) { ++*(int *)userdata; }
static void provider_test(void)
{
    knurl_mmdb_config config;
    knurl_mmdb_address address;
    knurl_mmdb_country country;
    knurl_mmdb *db = NULL;
    knurl_error error;
    int count = 0;
    const char *bad[] = {"localhost",  "1.2.3.4:80", "fe80::1%lo", "[::1]",
                         "1.2.3.4/32", "",           NULL};
    size_t i;
    knurl_mmdb_config_init(&config);
    assert(config.lookup == NULL && config.close == NULL &&
           config.userdata == NULL);
    assert(knurl_mmdb_new(&config, &db, &error) == KNURL_INVALID && db == NULL);
    config.lookup = custom;
    config.close = closed;
    config.userdata = &count;
    assert(knurl_mmdb_new(&config, NULL, &error) == KNURL_INVALID &&
           count == 0);
    assert(knurl_mmdb_new(&config, &db, &error) == KNURL_OK);
    expect(db, "::ffff:1.2.3.4", "SE", KNURL_OK);
    expect(db, "0.0.0.0", "", KNURL_BACKEND);
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        assert(knurl_mmdb_address_parse(bad[i], &address, &error) ==
               KNURL_INVALID);
        assert(address.size == 0);
    }
    assert(db->lookup(db, &address, &country, &error) == KNURL_INVALID);
    assert(knurl_mmdb_lookup(NULL, &address, &country, &error) ==
           KNURL_INVALID);
    assert(db->close(db, &error) == KNURL_OK && count == 1);
    assert(knurl_mmdb_close(NULL, &error) == KNURL_OK);
}
static void *thread_lookup(void *userdata)
{
    size_t i;
    for (i = 0; i < 1000; ++i)
        expect((knurl_mmdb *)userdata, "1.2.3.4", "US", KNURL_OK);
    return NULL;
}
static void *thread_skip(void *userdata)
{
    expect((knurl_mmdb *)userdata, "1.2.3.4", "DE", KNURL_OK);
    return NULL;
}
static void *thread_cache(void *userdata)
{
    unsigned int i;
    char ip[32];
    for (i = 0; i < 256; ++i) {
        assert(snprintf(ip, sizeof(ip), "100.0.%u.1", i % 128) <
               (int)sizeof(ip));
        expect((knurl_mmdb *)userdata, ip, i % 2 ? "SE" : "US", KNURL_OK);
    }
    return NULL;
}
static void cache_test(void)
{
    knurl_mmdb_file_config config;
    knurl_mmdb *db;
    knurl_error error;
    char path[1024], ip[32];
    size_t limits[] = {8192u, 65536u, 262144u};
    size_t n;
    unsigned int i, round, index;
    pthread_t a, b;
    knurl_mmdb_file_config_init(&config);
    assert(config.max_heap_bytes == 0);
    config.max_heap_bytes = KNURL_MMDB_HEAP_MIN - 1;
    assert(knurl_mmdb_open("unused", &config, &db, &error) == KNURL_INVALID &&
           db == NULL);
    assert(snprintf(path, sizeof(path), "%s/cache.mmdb", root) <
           (int)sizeof(path));
    for (n = 0; n < sizeof(limits) / sizeof(limits[0]); ++n) {
        config.max_heap_bytes = limits[n];
        assert(knurl_mmdb_open(path, &config, &db, &error) == KNURL_OK);
        for (round = 0; round < 3; ++round)
            for (i = 0; i < 128; ++i) {
                index = round % 2 ? 127 - i : i;
                assert(snprintf(ip, sizeof(ip), "100.0.%u.1", index) <
                       (int)sizeof(ip));
                expect(db, ip, index % 2 ? "SE" : "US", KNURL_OK);
            }
        assert(pthread_create(&a, NULL, thread_cache, db) == 0);
        assert(pthread_create(&b, NULL, thread_cache, db) == 0);
        assert(pthread_join(a, NULL) == 0 && pthread_join(b, NULL) == 0);
        db->close(db, NULL);
    }
    assert(snprintf(path, sizeof(path), "%s/boundary-code.mmdb", root) <
           (int)sizeof(path));
    config.max_heap_bytes = KNURL_MMDB_HEAP_MIN;
    assert(knurl_mmdb_open(path, &config, &db, &error) == KNURL_OK);
    expect(db, "1.2.3.4", "US", KNURL_OK);
    db->close(db, NULL);
}
static void prefix_test(void)
{
    knurl_mmdb *a, *b;
    size_t i;
    open_fixture("prefix-4", &a);
    for (i = 0; i < 3; ++i) {
        expect(a, "1.2.3.4", "US", KNURL_OK);
        expect(a, "1.2.3.5", "SE", KNURL_OK);
        expect(a, "1.2.3.6", "", KNURL_EOF);
        expect(a, "2.3.4.0", "US", KNURL_OK);
        expect(a, "2.3.4.127", "US", KNURL_OK);
        expect(a, "2.3.4.128", "", KNURL_EOF);
        expect(a, "::ffff:1.2.3.5", "SE", KNURL_OK);
        expect(a, "2001:db8::1", "", KNURL_EOF);
        expect(a, "2001:db8::2", "", KNURL_EOF);
    }
    a->close(a, NULL);
    open_fixture("prefix-6", &a);
    for (i = 0; i < 3; ++i) {
        expect(a, "2001:db8::1", "US", KNURL_OK);
        expect(a, "2001:db8::2", "SE", KNURL_OK);
        expect(a, "2001:db8::3", "", KNURL_EOF);
        expect(a, "2001:db8:1::", "US", KNURL_OK);
        expect(a, "2001:db8:1::1", "US", KNURL_OK);
        expect(a, "2001:db8:1::2", "", KNURL_EOF);
        expect(a, "1.2.3.4", "", KNURL_EOF);
    }
    a->close(a, NULL);
    open_fixture("cache", &a);
    open_fixture("cache-new", &b);
    expect(a, "100.0.0.1", "US", KNURL_OK);
    expect(b, "100.0.0.1", "SE", KNURL_OK);
    expect(a, "100.0.0.255", "US", KNURL_OK);
    b->close(b, NULL);
    expect(a, "100.0.0.0", "US", KNURL_OK);
    a->close(a, NULL);
}
int main(int argc, char **argv)
{
    knurl_mmdb *db;
    knurl_error error;
    char name[64], path[1024];
    unsigned int width, version;
    size_t i;
    pthread_t a, b;
    const char *valid[] = {"pointer-1",  "pointer-2",    "pointer-3",
                           "pointer-4",  "skip-28",      "skip-29",
                           "skip-284",   "skip-285",     "skip-65820",
                           "skip-65821", "skip-scalars", "skip-depth"};
    const char *invalid[] = {"empty", "truncated", "bad-metadata",
                             "bad-version", "missing-metadata"};
    const char *bad_lookup[] = {"bad-code", "bad-type", "bad-case",
                                "bad-pointer", "pointer-cycle"};
    assert(argc == 2);
    root = argv[1];
    provider_test();
    cache_test();
    prefix_test();
    for (width = 24; width <= 32; width += 4)
        for (version = 4; version <= 6; version += 2) {
            sprintf(name, "valid-%u-%u", width, version);
            open_fixture(name, &db);
            expect(db, "1.2.3.4", "US", KNURL_OK);
            expect(db, "1.2.3.0", "US", KNURL_OK);
            expect(db, "1.2.3.255", "US", KNURL_OK);
            expect(db, "1.2.2.255", "", KNURL_EOF);
            expect(db, "1.2.4.0", "", KNURL_EOF);
            expect(db, "::ffff:1.2.3.4", "US", KNURL_OK);
            expect(db, "128.0.0.1", "US", KNURL_OK);
            expect(db, "255.255.255.255", "US", KNURL_OK);
            expect(db, "127.255.255.255", "", KNURL_EOF);
            expect(db, "2001:db8::1", version == 6 ? "US" : "",
                   version == 6 ? KNURL_OK : KNURL_EOF);
            expect(db, "2001:db8:ffff:ffff:ffff:ffff:ffff:ffff",
                   version == 6 ? "US" : "",
                   version == 6 ? KNURL_OK : KNURL_EOF);
            expect(db, "2001:db9::", "", KNURL_EOF);
            assert(pthread_create(&a, NULL, thread_lookup, db) == 0);
            assert(pthread_create(&b, NULL, thread_lookup, db) == 0);
            assert(pthread_join(a, NULL) == 0 && pthread_join(b, NULL) == 0);
            assert(db->close(db, &error) == KNURL_OK);
        }
    for (i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i) {
        open_fixture(valid[i], &db);
        expect(db, "1.2.3.4", i < 4 ? "SE" : "DE", KNURL_OK);
        if (strcmp(valid[i], "skip-depth") == 0) {
            assert(pthread_create(&a, NULL, thread_skip, db) == 0);
            assert(pthread_join(a, NULL) == 0);
        }
        db->close(db, NULL);
    }
    for (i = 0; i < 2; ++i) {
        open_fixture(i ? "missing-code" : "missing", &db);
        expect(db, "1.2.3.4", "", KNURL_EOF);
        db->close(db, NULL);
    }
    for (i = 0; i < sizeof(bad_lookup) / sizeof(bad_lookup[0]); ++i) {
        open_fixture(bad_lookup[i], &db);
        expect(db, "1.2.3.4", "", KNURL_BACKEND);
        expect(db, "1.2.3.5", "", KNURL_BACKEND);
        db->close(db, NULL);
    }
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        assert(snprintf(path, sizeof(path), "%s/%s.mmdb", root, invalid[i]) <
               (int)sizeof(path));
        assert(open_db(path, &db, &error) != KNURL_OK && db == NULL);
    }
    assert(open_db(NULL, &db, &error) == KNURL_INVALID && db == NULL);
    assert(open_db("/knurl-no-such-mmdb", &db, &error) == KNURL_IO &&
           db == NULL);
    {
        const char *native_bad[] = {"duplicate-metadata",
                                    "duplicate-description", "bad-separator",
                                    "many-metadata"};
        const char *native_bad_lookup[] = {
            "null-root",       "bad-integer",     "bad-double",
            "bad-boolean",     "bad-size-29",     "bad-size-30",
            "bad-size-31",     "huge-array",      "short-pointer-1",
            "short-pointer-2", "short-pointer-3", "short-pointer-4",
            "bad-tree-data",   "tree-cycle",      "bad-tree-offset"};
        for (i = 0; i < sizeof(native_bad) / sizeof(native_bad[0]); ++i) {
            assert(snprintf(path, sizeof(path), "%s/%s.mmdb", root,
                            native_bad[i]) < (int)sizeof(path));
            assert(open_db(path, &db, &error) == KNURL_BACKEND && db == NULL);
        }
        for (i = 0;
             i < sizeof(native_bad_lookup) / sizeof(native_bad_lookup[0]);
             ++i) {
            open_fixture(native_bad_lookup[i], &db);
            expect(db, "1.2.3.4", "", KNURL_BACKEND);
            db->close(db, NULL);
        }
        open_fixture("optional-metadata", &db);
        expect(db, "1.2.3.4", "US", KNURL_OK);
        db->close(db, NULL);
        assert(snprintf(path, sizeof(path), "%s/duplicate-metadata.mmdb",
                        root) < (int)sizeof(path));
        assert(open_db(path, &db, &error) == KNURL_BACKEND && db == NULL);
    }
    {
        const char *extra[] = {"high-28",          "high-32",
                               "root-pointer",     "key-pointer",
                               "boundary-control", "boundary-code"};
        for (i = 0; i < sizeof(extra) / sizeof(extra[0]); ++i) {
            open_fixture(extra[i], &db);
            expect(db, "1.2.3.4", "US", KNURL_OK);
            db->close(db, NULL);
        }
        assert(snprintf(path, sizeof(path), "%s/huge-node-count.mmdb", root) <
               (int)sizeof(path));
        assert(open_db(path, &db, &error) != KNURL_OK && db == NULL);
        assert(snprintf(path, sizeof(path), "%s/bad-record-width.mmdb", root) <
               (int)sizeof(path));
        assert(open_db(path, &db, &error) != KNURL_OK && db == NULL);
    }
    return 0;
}
