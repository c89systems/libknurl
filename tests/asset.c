#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include <knurl/knurl.h>
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
    knurl_asset *asset;
    knurl_u64 offset;
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
static knurl_status reader(knurl_u64 offset, void *bytes, size_t capacity,
                           size_t *n, void *data, knurl_error *error)
{
    struct state *s = data;
    (void)error;
    s->read++;
    s->offset = offset;
    *n = 0;
    if (s->mode == 1)
        return KNURL_AGAIN;
    if (s->mode == 2)
        return KNURL_EOF;
    if (s->mode == 3) {
        *n = capacity + 1;
        return KNURL_OK;
    }
    if (s->mode == 4) {
        s->asset->close(s->asset, error);
        return KNURL_OK;
    }
    *n = capacity;
    memset(bytes, 'x', *n);
    return KNURL_OK;
}
static knurl_status opener(const char *path, knurl_asset **out, void *data,
                           knurl_error *error)
{
    struct state *s = data;
    knurl_asset_config config;
    s->opened++;
    if (strcmp(path, "/file") != 0)
        return KNURL_EOF;
    knurl_asset_config_init(&config);
    config.info.size.high = 1;
    config.info.size.low = 16;
    config.info.etag = "\"snapshot\"";
    config.read = reader;
    config.close = asset_closed;
    config.userdata = s;
    return knurl_asset_new(&config, out, error);
}
static int custom(void)
{
    struct state s;
    knurl_error error;
    knurl_asset_provider_config config;
    knurl_asset_provider *p;
    knurl_asset *a;
    knurl_source *source;
    knurl_u64 start, length;
    char bytes[16];
    size_t n;
    memset(&s, 0, sizeof(s));
    knurl_error_clear(&error);
    knurl_asset_provider_config_init(&config);
    config.open = opener;
    config.close = provider_closed;
    config.userdata = &s;
    CHECK(knurl_asset_provider_new(&config, &p, &error) == KNURL_OK &&
          s.opened == 0);
    CHECK(p->open(p, "/%252e", &a, &error) == KNURL_INVALID && s.opened == 0);
    CHECK(p->open(p, "/../file", &a, &error) == KNURL_INVALID);
    CHECK(p->open(p, "/file/", &a, &error) == KNURL_INVALID);
    CHECK(p->open(p, "/missing", &a, &error) == KNURL_EOF && a == NULL);
    CHECK(p->open(p, "/file", &a, &error) == KNURL_OK);
    start.high = 1;
    start.low = 4;
    length.high = 0;
    length.low = 3;
    CHECK(a->source_take(a, start, length, &source, &error) == KNURL_OK &&
          s.read == 0);
    CHECK(a->close(a, &error) == KNURL_STATE);
    CHECK(p->close(p, &error) == KNURL_OK && s.provider_closed == 0);
    s.mode = 1;
    CHECK(source->read(source, bytes, sizeof(bytes), &n, &error) ==
              KNURL_AGAIN &&
          n == 0);
    s.mode = 0;
    CHECK(source->read(source, bytes, sizeof(bytes), &n, &error) == KNURL_OK &&
          n == 3 && s.offset.high == 1 && s.offset.low == 4);
    CHECK(source->read(source, bytes, sizeof(bytes), &n, &error) == KNURL_EOF &&
          n == 0);
    CHECK(source->reset(source, &error) == KNURL_OK);
    CHECK(source->read(source, bytes, 1, &n, &error) == KNURL_OK && n == 1 &&
          s.offset.high == 1 && s.offset.low == 4);
    CHECK(source->destroy(source, &error) == KNURL_OK && s.closed == 1 &&
          s.provider_closed == 1);
    CHECK(knurl_asset_provider_new(&config, &p, &error) == KNURL_OK);
    CHECK(p->open(p, "/file", &a, &error) == KNURL_OK);
    start.high = start.low = 0;
    s.mode = 2;
    CHECK(a->read(a, start, bytes, sizeof(bytes), &n, &error) == KNURL_IO &&
          n == 0);
    CHECK(a->close(a, &error) == KNURL_OK);
    CHECK(p->open(p, "/file", &a, &error) == KNURL_OK);
    s.mode = 3;
    CHECK(a->read(a, start, bytes, sizeof(bytes), &n, &error) ==
              KNURL_INVALID &&
          n == 0);
    CHECK(a->close(a, &error) == KNURL_OK);
    CHECK(p->open(p, "/file", &a, &error) == KNURL_OK);
    s.asset = a;
    s.mode = 4;
    CHECK(a->read(a, start, bytes, sizeof(bytes), &n, &error) == KNURL_STATE &&
          n == 0);
    CHECK(a->close(a, &error) == KNURL_OK);
    CHECK(p->close(p, &error) == KNURL_OK);
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
    knurl_asset_provider *p;
    knurl_asset *a, *listing;
    knurl_asset_entry entry;
    knurl_error error;
    knurl_u64 zero;
    knurl_status status;
    size_t n;
    int count = 0;
    knurl_error_clear(&error);
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
    CHECK(knurl_asset_filesystem_new(root, &p, &error) == KNURL_OK);
    CHECK(p->open(p, "/link", &a, &error) == KNURL_EOF);
    CHECK(p->open(p, "/pipe", &a, &error) == KNURL_EOF);
    CHECK(p->open(p, "/file", &a, &error) == KNURL_OK &&
          a->stat(a)->size.low == 8 && a->stat(a)->last_modified != NULL);
    snprintf(etag, sizeof(etag), "%s", a->stat(a)->etag);
    snprintf(path, sizeof(path), "%s/file", root);
    CHECK(unlink(path) == 0 && put(path, "replaced"));
    CHECK(a->read(a, zero, bytes, sizeof(bytes), &n, &error) == KNURL_OK &&
          n == 8 && memcmp(bytes, "original", 8) == 0 &&
          strcmp(etag, a->stat(a)->etag) == 0);
    CHECK(a->close(a, &error) == KNURL_OK);
    CHECK(rename(root, moved) == 0 && mkdir(root, 0700) == 0);
    snprintf(path, sizeof(path), "%s/file", root);
    CHECK(put(path, "new-root"));
    CHECK(p->open(p, "/file", &a, &error) == KNURL_OK);
    CHECK(a->read(a, zero, bytes, sizeof(bytes), &n, &error) == KNURL_OK &&
          n == 8 && memcmp(bytes, "replaced", 8) == 0);
    CHECK(a->close(a, &error) == KNURL_OK);
    CHECK(p->open(p, "/", &listing, &error) == KNURL_OK);
    while ((status = listing->list_next(listing, &entry, &error)) == KNURL_OK ||
           status == KNURL_AGAIN) {
        if (status == KNURL_AGAIN)
            continue;
        CHECK(strcmp(entry.name, "file") == 0);
        count++;
    }
    CHECK(status == KNURL_EOF && count == 1 &&
          listing->list_next(listing, &entry, &error) == KNURL_EOF);
    CHECK(p->close(p, &error) == KNURL_OK &&
          listing->close(listing, &error) == KNURL_OK);
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
    knurl_router *router;
    knurl_static_config config;
    knurl_asset_provider *provider;
    knurl_asset *asset;
    knurl_error error;
    CHECK(knurl_asset_filesystem_new("asset-missing-root-should-not-exist",
                                     &provider, &error) == KNURL_OK);
    CHECK(provider->open(provider, "/missing", &asset, &error) == KNURL_EOF &&
          asset == NULL);
    CHECK(provider->close(provider, &error) == KNURL_OK);
    CHECK(knurl_router_new(&router, &error) == KNURL_OK);
    knurl_static_config_init(&config);
    config.path = "/files";
    config.root_dir = "asset-missing-root-should-not-exist";
    CHECK(config.index_enabled == 0 &&
          router->static_directory(router, &config, NULL, &error) == KNURL_OK);
    config.path = "/bad";
    config.methods = KNURL_HTTP_METHODS_POST;
    CHECK(router->static_directory(router, &config, NULL, &error) ==
              KNURL_INVALID &&
          router->route_count(router) == 1);
    config.methods = 0;
    config.index_file = "../index.html";
    CHECK(router->static_directory(router, &config, NULL, &error) ==
              KNURL_INVALID &&
          router->route_count(router) == 1);
    CHECK(router->destroy(router, &error) == KNURL_OK);
    CHECK(strcmp(knurl_static_content_type("/a.HTML"),
                 "text/html; charset=utf-8") == 0);
    CHECK(strcmp(knurl_static_content_type("/a.mjs"),
                 "text/javascript; charset=utf-8") == 0);
    CHECK(strcmp(knurl_static_content_type("/README.md"),
                 "text/markdown; charset=utf-8") == 0);
    CHECK(strcmp(knurl_static_content_type("/README.MD"),
                 "text/markdown; charset=utf-8") == 0);
    CHECK(strcmp(knurl_static_content_type("/guide.markdown"),
                 "text/markdown; charset=utf-8") == 0);
    CHECK(strcmp(knurl_static_content_type("/guide.MarkDown"),
                 "text/markdown; charset=utf-8") == 0);
    CHECK(strcmp(knurl_static_content_type("/README.md.txt"),
                 "text/plain; charset=utf-8") == 0);
    CHECK(strcmp(knurl_static_content_type("/folder.md/README"),
                 "application/octet-stream") == 0);
    CHECK(strcmp(knurl_static_content_type("/a.unknown"),
                 "application/octet-stream") == 0);
    return 0;
}
static knurl_status list_name(knurl_asset_entry *entry, void *data,
                              knurl_error *error)
{
    (void)error;
    entry->name = data;
    return KNURL_OK;
}
static int listing_names(void)
{
    static const char *names[] = {
        "a %?\\ b.txt", "../outside", "/absolute", ".",
        "..",           "x/y",        "bad\nname"};
    knurl_asset_config config;
    knurl_asset *asset;
    knurl_asset_entry entry;
    knurl_error error;
    size_t i;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        knurl_asset_config_init(&config);
        config.info.kind = KNURL_ASSET_DIRECTORY;
        config.next = list_name;
        config.userdata = (void *)names[i];
        CHECK(knurl_asset_new(&config, &asset, &error) == KNURL_OK);
        CHECK(asset->list_next(asset, &entry, &error) ==
              (i ? KNURL_INVALID : KNURL_OK));
        if (!i)
            CHECK(strcmp(entry.name, names[i]) == 0);
        CHECK(asset->close(asset, &error) == KNURL_OK);
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
    knurl_error error;
    knurl_error_clear(&error);
    CHECK(pthread_create(&thread, NULL, run, &result) == 0 &&
          pthread_join(thread, NULL) == 0);
    return result;
}
