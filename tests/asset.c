#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include <rotmos/rotmos.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(t)                                                               \
    do {                                                                       \
        if (!(t)) {                                                            \
            fprintf(stderr, "asset %d: %s (%s)\n", __LINE__, #t,               \
                    error.message);                                            \
            return 1;                                                          \
        }                                                                      \
    } while (0)
struct state {
    int opened, closed, provider_closed, read, mode;
    rotmos_asset *asset;
    rotmos_u64 offset;
};
static void asset_closed(void *data)
{
    struct state *s = data;
    s->closed++;
}
static void provider_closed(void *data)
{
    struct state *s = data;
    s->provider_closed++;
}
static rotmos_status reader(rotmos_u64 offset, void *bytes, size_t capacity,
                            size_t *n, void *data, rotmos_error *error)
{
    struct state *s = data;
    (void)error;
    s->read++;
    s->offset = offset;
    *n = 0;
    if (s->mode == 1)
        return ROTMOS_AGAIN;
    if (s->mode == 2)
        return ROTMOS_EOF;
    if (s->mode == 3) {
        *n = capacity + 1;
        return ROTMOS_OK;
    }
    if (s->mode == 4) {
        rotmos_asset_close(s->asset, error);
        return ROTMOS_OK;
    }
    *n = capacity;
    memset(bytes, 'x', *n);
    return ROTMOS_OK;
}
static rotmos_status opener(const char *path, rotmos_asset **out, void *data,
                            rotmos_error *error)
{
    struct state *s = data;
    rotmos_asset_config config;
    s->opened++;
    if (strcmp(path, "/file") != 0)
        return ROTMOS_EOF;
    rotmos_asset_config_init(&config);
    config.info.size.high = 1;
    config.info.size.low = 16;
    config.info.etag = "\"snapshot\"";
    config.read = reader;
    config.close = asset_closed;
    config.userdata = s;
    return rotmos_asset_new(&config, out, error);
}
static int custom(void)
{
    struct state s;
    rotmos_error error;
    rotmos_asset_provider_config config;
    rotmos_asset_provider *p;
    rotmos_asset *a;
    rotmos_source *source;
    rotmos_u64 start, length;
    char bytes[16];
    size_t n;
    memset(&s, 0, sizeof(s));
    rotmos_error_clear(&error);
    rotmos_asset_provider_config_init(&config);
    config.open = opener;
    config.close = provider_closed;
    config.userdata = &s;
    CHECK(rotmos_asset_provider_new(&config, &p, &error) == ROTMOS_OK &&
          s.opened == 0);
    CHECK(rotmos_asset_provider_open(p, "/%252e", &a, &error) ==
              ROTMOS_INVALID &&
          s.opened == 0);
    CHECK(rotmos_asset_provider_open(p, "/../file", &a, &error) ==
          ROTMOS_INVALID);
    CHECK(rotmos_asset_provider_open(p, "/file/", &a, &error) ==
          ROTMOS_INVALID);
    CHECK(rotmos_asset_provider_open(p, "/missing", &a, &error) == ROTMOS_EOF &&
          a == NULL);
    CHECK(rotmos_asset_provider_open(p, "/file", &a, &error) == ROTMOS_OK);
    start.high = 1;
    start.low = 4;
    length.high = 0;
    length.low = 3;
    CHECK(rotmos_asset_source_take(a, start, length, &source, &error) ==
              ROTMOS_OK &&
          s.read == 0);
    CHECK(rotmos_asset_close(a, &error) == ROTMOS_STATE);
    CHECK(rotmos_asset_provider_close(p, &error) == ROTMOS_OK &&
          s.provider_closed == 0);
    s.mode = 1;
    CHECK(rotmos_source_read(source, bytes, sizeof(bytes), &n, &error) ==
              ROTMOS_AGAIN &&
          n == 0);
    s.mode = 0;
    CHECK(rotmos_source_read(source, bytes, sizeof(bytes), &n, &error) ==
              ROTMOS_OK &&
          n == 3 && s.offset.high == 1 && s.offset.low == 4);
    CHECK(rotmos_source_read(source, bytes, sizeof(bytes), &n, &error) ==
              ROTMOS_EOF &&
          n == 0);
    CHECK(rotmos_source_reset(source, &error) == ROTMOS_OK);
    CHECK(rotmos_source_read(source, bytes, 1, &n, &error) == ROTMOS_OK &&
          n == 1 && s.offset.high == 1 && s.offset.low == 4);
    CHECK(rotmos_source_destroy(source, &error) == ROTMOS_OK && s.closed == 1 &&
          s.provider_closed == 1);
    CHECK(rotmos_asset_provider_new(&config, &p, &error) == ROTMOS_OK);
    CHECK(rotmos_asset_provider_open(p, "/file", &a, &error) == ROTMOS_OK);
    start.high = start.low = 0;
    s.mode = 2;
    CHECK(rotmos_asset_read(a, start, bytes, sizeof(bytes), &n, &error) ==
              ROTMOS_IO &&
          n == 0);
    CHECK(rotmos_asset_close(a, &error) == ROTMOS_OK);
    CHECK(rotmos_asset_provider_open(p, "/file", &a, &error) == ROTMOS_OK);
    s.mode = 3;
    CHECK(rotmos_asset_read(a, start, bytes, sizeof(bytes), &n, &error) ==
              ROTMOS_INVALID &&
          n == 0);
    CHECK(rotmos_asset_close(a, &error) == ROTMOS_OK);
    CHECK(rotmos_asset_provider_open(p, "/file", &a, &error) == ROTMOS_OK);
    s.asset = a;
    s.mode = 4;
    CHECK(rotmos_asset_read(a, start, bytes, sizeof(bytes), &n, &error) ==
              ROTMOS_STATE &&
          n == 0);
    CHECK(rotmos_asset_close(a, &error) == ROTMOS_OK);
    CHECK(rotmos_asset_provider_close(p, &error) == ROTMOS_OK);
    return 0;
}
static int put(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    size_t n = strlen(text);
    if (fd < 0)
        return 0;
    if (write(fd, text, n) != (ssize_t)n) {
        close(fd);
        return 0;
    }
    return close(fd) == 0;
}
static int filesystem(void)
{
    char directory[] = "asset-fixture-XXXXXX", root[256], moved[256], path[512],
         other[512], bytes[16], etag[256];
    rotmos_asset_provider *p;
    rotmos_asset *a, *listing;
    rotmos_asset_entry entry;
    rotmos_error error;
    rotmos_u64 zero;
    rotmos_status status;
    size_t n;
    int count = 0;
    rotmos_error_clear(&error);
    zero.high = zero.low = 0;
    CHECK(mkdtemp(directory) != NULL);
    snprintf(root, sizeof(root), "%s/root", directory);
    snprintf(moved, sizeof(moved), "%s/moved", directory);
    CHECK(mkdir(root, 0700) == 0);
    snprintf(path, sizeof(path), "%s/file", root);
    CHECK(put(path, "original"));
    snprintf(other, sizeof(other), "%s/secret", directory);
    CHECK(put(other, "outside"));
    snprintf(path, sizeof(path), "%s/link", root);
    CHECK(symlink("../secret", path) == 0);
    snprintf(path, sizeof(path), "%s/pipe", root);
    CHECK(mkfifo(path, 0600) == 0);
    CHECK(rotmos_asset_filesystem_new(root, &p, &error) == ROTMOS_OK);
    CHECK(rotmos_asset_provider_open(p, "/link", &a, &error) == ROTMOS_EOF);
    CHECK(rotmos_asset_provider_open(p, "/pipe", &a, &error) == ROTMOS_EOF);
    CHECK(rotmos_asset_provider_open(p, "/file", &a, &error) == ROTMOS_OK &&
          rotmos_asset_stat(a)->size.low == 8 &&
          rotmos_asset_stat(a)->last_modified != NULL);
    snprintf(etag, sizeof(etag), "%s", rotmos_asset_stat(a)->etag);
    snprintf(path, sizeof(path), "%s/file", root);
    CHECK(unlink(path) == 0 && put(path, "replaced"));
    CHECK(rotmos_asset_read(a, zero, bytes, sizeof(bytes), &n, &error) ==
              ROTMOS_OK &&
          n == 8 && memcmp(bytes, "original", 8) == 0 &&
          strcmp(etag, rotmos_asset_stat(a)->etag) == 0);
    CHECK(rotmos_asset_close(a, &error) == ROTMOS_OK);
    CHECK(rename(root, moved) == 0 && mkdir(root, 0700) == 0);
    snprintf(path, sizeof(path), "%s/file", root);
    CHECK(put(path, "new-root"));
    CHECK(rotmos_asset_provider_open(p, "/file", &a, &error) == ROTMOS_OK);
    CHECK(rotmos_asset_read(a, zero, bytes, sizeof(bytes), &n, &error) ==
              ROTMOS_OK &&
          n == 8 && memcmp(bytes, "replaced", 8) == 0);
    CHECK(rotmos_asset_close(a, &error) == ROTMOS_OK);
    CHECK(rotmos_asset_provider_open(p, "/", &listing, &error) == ROTMOS_OK);
    while ((status = rotmos_asset_list_next(listing, &entry, &error)) ==
               ROTMOS_OK ||
           status == ROTMOS_AGAIN) {
        if (status == ROTMOS_AGAIN)
            continue;
        CHECK(strcmp(entry.name, "file") == 0);
        count++;
    }
    CHECK(status == ROTMOS_EOF && count == 1 &&
          rotmos_asset_list_next(listing, &entry, &error) == ROTMOS_EOF);
    CHECK(rotmos_asset_provider_close(p, &error) == ROTMOS_OK &&
          rotmos_asset_close(listing, &error) == ROTMOS_OK);
    snprintf(path, sizeof(path), "%s/file", root);
    CHECK(unlink(path) == 0 && rmdir(root) == 0);
    snprintf(path, sizeof(path), "%s/file", moved);
    CHECK(unlink(path) == 0);
    snprintf(path, sizeof(path), "%s/link", moved);
    CHECK(unlink(path) == 0);
    snprintf(path, sizeof(path), "%s/pipe", moved);
    CHECK(unlink(path) == 0 && rmdir(moved) == 0);
    CHECK(unlink(other) == 0 && rmdir(directory) == 0);
    return 0;
}
static int declarations(void)
{
    rotmos_router *router;
    rotmos_static_config config;
    rotmos_asset_provider *provider;
    rotmos_asset *asset;
    rotmos_error error;
    CHECK(rotmos_asset_filesystem_new("asset-missing-root-should-not-exist",
                                      &provider, &error) == ROTMOS_OK);
    CHECK(rotmos_asset_provider_open(provider, "/missing", &asset, &error) ==
              ROTMOS_EOF &&
          asset == NULL);
    CHECK(rotmos_asset_provider_close(provider, &error) == ROTMOS_OK);
    CHECK(rotmos_router_new(&router, &error) == ROTMOS_OK);
    rotmos_static_config_init(&config);
    config.path = "/files";
    config.root_dir = "asset-missing-root-should-not-exist";
    CHECK(config.index_enabled == 0 &&
          rotmos_router_static_directory(router, &config, NULL, &error) ==
              ROTMOS_OK);
    config.path = "/bad";
    config.methods = ROTMOS_HTTP_METHODS_POST;
    CHECK(rotmos_router_static_directory(router, &config, NULL, &error) ==
              ROTMOS_INVALID &&
          rotmos_router_route_count(router) == 1);
    config.methods = 0;
    config.index_file = "../index.html";
    CHECK(rotmos_router_static_directory(router, &config, NULL, &error) ==
              ROTMOS_INVALID &&
          rotmos_router_route_count(router) == 1);
    CHECK(rotmos_router_destroy(router, &error) == ROTMOS_OK);
    CHECK(strcmp(rotmos_static_content_type("/a.HTML"),
                 "text/html; charset=utf-8") == 0);
    CHECK(strcmp(rotmos_static_content_type("/a.mjs"),
                 "text/javascript; charset=utf-8") == 0);
    CHECK(strcmp(rotmos_static_content_type("/a.unknown"),
                 "application/octet-stream") == 0);
    return 0;
}
static rotmos_status list_name(rotmos_asset_entry *entry, void *data,
                               rotmos_error *error)
{
    (void)error;
    entry->name = data;
    return ROTMOS_OK;
}
static int listing_names(void)
{
    static const char *names[] = {
        "a %?\\ b.txt", "../outside", "/absolute", ".",
        "..",           "x/y",        "bad\nname"};
    rotmos_asset_config config;
    rotmos_asset *asset;
    rotmos_asset_entry entry;
    rotmos_error error;
    size_t i;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        rotmos_asset_config_init(&config);
        config.info.kind = ROTMOS_ASSET_DIRECTORY;
        config.next = list_name;
        config.userdata = (void *)names[i];
        CHECK(rotmos_asset_new(&config, &asset, &error) == ROTMOS_OK);
        CHECK(rotmos_asset_list_next(asset, &entry, &error) ==
              (i ? ROTMOS_INVALID : ROTMOS_OK));
        if (!i)
            CHECK(strcmp(entry.name, names[i]) == 0);
        CHECK(rotmos_asset_close(asset, &error) == ROTMOS_OK);
    }
    return 0;
}
static void *run(void *data)
{
    int *result = data;
    *result = declarations();
    if (*result == 0)
        *result = custom();
    if (*result == 0)
        *result = filesystem();
    if (*result == 0)
        *result = listing_names();
    return NULL;
}
int main(void)
{
    pthread_t thread;
    int result = 1;
    rotmos_error error;
    rotmos_error_clear(&error);
    CHECK(pthread_create(&thread, NULL, run, &result) == 0 &&
          pthread_join(thread, NULL) == 0);
    return result;
}
