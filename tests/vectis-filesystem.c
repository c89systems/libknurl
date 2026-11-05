/* Standalone assertions lifted from Vectis 6b1e263e: unit/test_vectis_webdav.c
 * test_failed_move, test_delete_and_shallow_copy and test_conditional_put.
 * Calls are translated by vectis-filesystem.h through installed public APIs. */
#define _POSIX_C_SOURCE 200809L
#include "vectis-filesystem.h"
#include <sys/wait.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static int failures = 0;

static void expect(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "failure: %s\n", message);
        failures++;
    }
}

static void remove_tree(const char *path)
{
    DIR *directory;
    struct dirent *item;
    struct stat st;
    char child[4096];
    int written;

    directory = opendir(path);
    if (directory == NULL) {
        return;
    }
    while ((item = readdir(directory)) != NULL) {
        if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0) {
            continue;
        }
        written = snprintf(child, sizeof(child), "%s/%s", path, item->d_name);
        if (written < 0 || (size_t)written >= sizeof(child) ||
            lstat(child, &st) == -1) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            remove_tree(child);
        } else {
            (void)unlink(child);
        }
    }
    (void)closedir(directory);
    (void)rmdir(path);
}

typedef struct list_state {
    int files;
    int collections;
} list_state;

static void test_failed_move(const reference_webdav_config *storage)
{
    char parent[4096];
    unsigned char *body;
    size_t size;
    reference_webdav_entry entry;
    int overwrite;
    pid_t child;
    int result;

    /* Drop root privileges in a child so permission failures are reproducible.
     */
    child = fork();
    expect(child >= 0, "fork permission regression");
    if (child == 0) {
        if (geteuid() == 0 && setuid(65534) != 0)
            _exit(2);
        expect(reference_webdav_mkcol(storage, "/locked") ==
                   REFERENCE_WEBDAV_OK,
               "create source parent");
        expect(reference_webdav_mkcol(storage, "/locked/source") ==
                   REFERENCE_WEBDAV_OK,
               "create source collection");
        for (overwrite = 0; overwrite < 2; ++overwrite) {
            expect(reference_webdav_put(storage, "/locked/source/file",
                                        (const unsigned char *)"precious",
                                        8u) == REFERENCE_WEBDAV_OK,
                   "create move source");
            if (overwrite) {
                expect(reference_webdav_mkcol(storage, "/destination") ==
                           REFERENCE_WEBDAV_OK,
                       "create overwrite collection");
                expect(reference_webdav_put(storage, "/destination/old",
                                            (const unsigned char *)"old",
                                            3u) == REFERENCE_WEBDAV_OK,
                       "create overwrite destination");
            }
            (void)snprintf(parent, sizeof(parent), "%s/locked",
                           storage->root_dir);
            expect(chmod(parent, 0500) == 0, "lock source parent");
            expect(reference_webdav_move(storage, "/locked/source",
                                         "/destination",
                                         overwrite) == REFERENCE_WEBDAV_IO,
                   "report incomplete source removal");
            body = NULL;
            size = 0u;
            expect(reference_webdav_read(storage, "/destination/file", &body,
                                         &size,
                                         &entry) == REFERENCE_WEBDAV_OK &&
                       size == 8u && memcmp(body, "precious", 8u) == 0,
                   "failed move preserves complete destination");
            free(body);
            expect(chmod(parent, 0700) == 0,
                   "restore source parent permissions");
            expect(reference_webdav_delete(storage, "/destination") ==
                       REFERENCE_WEBDAV_OK,
                   "clean regression destination");
        }
        _exit(failures ? 1 : 0);
    }
    if (child > 0) {
        expect(waitpid(child, &result, 0) == child && WIFEXITED(result) &&
                   WEXITSTATUS(result) == 0,
               "permission failure regressions pass");
    }
}

typedef struct conditional_writer {
    const reference_webdav_config *config;
    const char *etag;
    int gate;
    reference_webdav_status result;
} conditional_writer;

static void *conditional_write(void *arg)
{
    conditional_writer *writer = (conditional_writer *)arg;
    char token;
    if (read(writer->gate, &token, 1u) != 1)
        return NULL;
    writer->result = reference_webdav_put_conditional(
        writer->config, "/conditional.txt", (const unsigned char *)"winner", 6u,
        writer->etag, writer->etag == NULL ? "*" : NULL);
    return NULL;
}

static void *conditional_delete(void *arg)
{
    conditional_writer *writer = (conditional_writer *)arg;
    char token;
    if (read(writer->gate, &token, 1u) == 1)
        writer->result = reference_webdav_delete_conditional(
            writer->config, "/conditional.txt", writer->etag, NULL);
    return NULL;
}

static void *conditional_move(void *arg)
{
    conditional_writer *writer = (conditional_writer *)arg;
    char token;
    if (read(writer->gate, &token, 1u) == 1)
        writer->result = reference_webdav_move_conditional(
            writer->config, "/conditional.txt", "/conditional-moved.txt", 1,
            writer->etag, NULL);
    return NULL;
}

static void test_delete_and_shallow_copy(const reference_webdav_config *config)
{
    reference_webdav_entry entry;
    unsigned char *body;
    size_t size;
    char etag[67];
    conditional_writer writers[8];
    pthread_t threads[8];
    int gate[2], i, winners = 0;
    expect(reference_webdav_mkcol(config, "/missing-parent/child") ==
               REFERENCE_WEBDAV_CONFLICT,
           "MKCOL rejects missing parent");
    expect(reference_webdav_lookup(config, "/missing-parent", &entry) ==
               REFERENCE_WEBDAV_NOT_FOUND,
           "MKCOL does not create intermediate collections");
    expect(reference_webdav_mkcol(config, "/missing-parent") ==
               REFERENCE_WEBDAV_OK,
           "create parent explicitly");
    expect(reference_webdav_mkcol(config, "/missing-parent/child") ==
               REFERENCE_WEBDAV_OK,
           "MKCOL accepts existing parent");
    expect(reference_webdav_delete(config, "/missing-parent") ==
               REFERENCE_WEBDAV_OK,
           "cleanup parent collection");
    expect(reference_webdav_mkcol_conditional(config, "/conditional-col", "*",
                                              NULL) ==
               REFERENCE_WEBDAV_PRECONDITION,
           "MKCOL If-Match requires existing resource");
    expect(reference_webdav_lookup(config, "/conditional-col", &entry) ==
               REFERENCE_WEBDAV_NOT_FOUND,
           "failed MKCOL leaves collection absent");
    expect(reference_webdav_mkcol_conditional(config, "/conditional-col",
                                              "invalid",
                                              NULL) == REFERENCE_WEBDAV_INVALID,
           "MKCOL rejects malformed condition");
    expect(reference_webdav_mkcol_conditional(config, "/conditional-col", NULL,
                                              "*") == REFERENCE_WEBDAV_OK,
           "MKCOL If-None-Match creates missing collection");
    expect(reference_webdav_mkcol_conditional(config, "/conditional-col", NULL,
                                              "*") ==
               REFERENCE_WEBDAV_PRECONDITION,
           "MKCOL If-None-Match rejects existing collection");
    expect(reference_webdav_mkcol_conditional(config, "/conditional-col", "*",
                                              NULL) == REFERENCE_WEBDAV_EXISTS,
           "matching MKCOL condition retains existing-resource behavior");
    expect(reference_webdav_delete(config, "/conditional-col") ==
               REFERENCE_WEBDAV_OK,
           "cleanup conditional collection");
    expect(reference_webdav_put(config, "/conditional.txt",
                                (const unsigned char *)"new",
                                3u) == REFERENCE_WEBDAV_OK,
           "seed delete");
    expect(reference_webdav_mkcol(config, "/conditional.txt/child") ==
               REFERENCE_WEBDAV_CONFLICT,
           "MKCOL rejects non-collection parent");
    expect(reference_webdav_delete_conditional(config, "/conditional.txt",
                                               "\"stale\"", NULL) ==
               REFERENCE_WEBDAV_PRECONDITION,
           "stale delete denied");
    expect(reference_webdav_delete_conditional(config, "/conditional.txt", NULL,
                                               "*") ==
               REFERENCE_WEBDAV_PRECONDITION,
           "exclusive delete denied");
    expect(reference_webdav_read(config, "/conditional.txt", &body, &size,
                                 &entry) == REFERENCE_WEBDAV_OK,
           "failed delete preserves file");
    expect(size == 3u && memcmp(body, "new", 3u) == 0,
           "preserves new revision");
    free(body);
    snprintf(etag, sizeof(etag), "\"%s\"", entry.etag);
    expect(pipe(gate) == 0, "delete race gate");
    for (i = 0; i < 8; ++i) {
        writers[i].config = config;
        writers[i].etag = etag;
        writers[i].gate = gate[0];
        writers[i].result = REFERENCE_WEBDAV_IO;
        expect(pthread_create(&threads[i], NULL, conditional_delete,
                              &writers[i]) == 0,
               "start conditional deleter");
    }
    expect(write(gate[1], "12345678", 8u) == 8, "release deleters");
    for (i = 0; i < 8; ++i) {
        expect(pthread_join(threads[i], NULL) == 0, "join deleter");
        if (writers[i].result == REFERENCE_WEBDAV_OK)
            ++winners;
        else
            expect(writers[i].result == REFERENCE_WEBDAV_PRECONDITION,
                   "delete loser");
    }
    close(gate[0]);
    close(gate[1]);
    expect(winners == 1, "one atomic conditional delete wins");
    expect(reference_webdav_put(config, "/conditional.txt",
                                (const unsigned char *)"new",
                                3u) == REFERENCE_WEBDAV_OK,
           "seed conditional transfer");
    expect(reference_webdav_copy_conditional(
               config, "/conditional.txt", "/conditional-moved.txt", 1, -1,
               "\"stale\"", NULL) == REFERENCE_WEBDAV_PRECONDITION,
           "reject stale copy");
    expect(reference_webdav_lookup(config, "/conditional-moved.txt", &entry) ==
               REFERENCE_WEBDAV_NOT_FOUND,
           "rejected copy creates nothing");
    expect(reference_webdav_copy_conditional(config, "/conditional.txt",
                                             "/conditional-moved.txt", 1, -1,
                                             etag, NULL) == REFERENCE_WEBDAV_OK,
           "accept matching copy");
    expect(pipe(gate) == 0, "move race gate");
    winners = 0;
    for (i = 0; i < 8; ++i) {
        writers[i].gate = gate[0];
        writers[i].result = REFERENCE_WEBDAV_IO;
        expect(pthread_create(&threads[i], NULL, conditional_move,
                              &writers[i]) == 0,
               "start mover");
    }
    expect(write(gate[1], "12345678", 8u) == 8, "release movers");
    for (i = 0; i < 8; ++i) {
        expect(pthread_join(threads[i], NULL) == 0, "join mover");
        if (writers[i].result == REFERENCE_WEBDAV_OK)
            ++winners;
        else
            expect(writers[i].result == REFERENCE_WEBDAV_PRECONDITION,
                   "move loser");
    }
    close(gate[0]);
    close(gate[1]);
    expect(winners == 1, "one atomic conditional move wins");
    expect(reference_webdav_read(config, "/conditional-moved.txt", &body, &size,
                                 &entry) == REFERENCE_WEBDAV_OK,
           "read moved data");
    expect(size == 3u && memcmp(body, "new", 3u) == 0, "moved data preserved");
    free(body);
    expect(reference_webdav_delete(config, "/conditional-moved.txt") ==
               REFERENCE_WEBDAV_OK,
           "clean moved fixture");
    expect(reference_webdav_mkcol(config, "/depth-source") ==
               REFERENCE_WEBDAV_OK,
           "depth source");
    expect(reference_webdav_put(config, "/depth-source/child",
                                (const unsigned char *)"x",
                                1u) == REFERENCE_WEBDAV_OK,
           "depth child");
    expect(reference_webdav_copy_depth(config, "/depth-source", "/depth-copy",
                                       0, 0) == REFERENCE_WEBDAV_OK,
           "shallow copy");
    expect(reference_webdav_lookup(config, "/depth-copy", &entry) ==
                   REFERENCE_WEBDAV_OK &&
               entry.kind == REFERENCE_WEBDAV_ENTRY_COLLECTION,
           "copies collection");
    expect(reference_webdav_lookup(config, "/depth-copy/child", &entry) ==
               REFERENCE_WEBDAV_NOT_FOUND,
           "does not copy descendants");
    expect(reference_webdav_copy(config, "/depth-source", "/depth-copy", 1) ==
               REFERENCE_WEBDAV_OK,
           "recursive overwrite");
    expect(reference_webdav_lookup(config, "/depth-copy/child", &entry) ==
               REFERENCE_WEBDAV_OK,
           "recursive child exists");
    expect(reference_webdav_copy_depth(config, "/depth-source", "/depth-copy",
                                       1, 0) == REFERENCE_WEBDAV_OK,
           "shallow overwrite");
    expect(reference_webdav_lookup(config, "/depth-copy/child", &entry) ==
               REFERENCE_WEBDAV_NOT_FOUND,
           "shallow overwrite removes old descendants");
    expect(reference_webdav_delete(config, "/depth-copy") ==
               REFERENCE_WEBDAV_OK,
           "clean copy");
    expect(reference_webdav_delete(config, "/depth-source") ==
               REFERENCE_WEBDAV_OK,
           "clean source");
}

static void test_conditional_put(const reference_webdav_config *config)
{
    unsigned char *body;
    size_t size;
    reference_webdav_entry entry;
    char etag[67], weak[70], list[90];
    pthread_t threads[8];
    pid_t children[8];
    conditional_writer writers[8];
    int gate[2], winners, phase, i, child_status;
    expect(reference_webdav_put_conditional(
               config, "/conditional.txt", (const unsigned char *)"old", 3u,
               "*", NULL) == REFERENCE_WEBDAV_PRECONDITION,
           "If-Match wildcard rejects missing resource");
    for (phase = 0; phase < 2; ++phase) {
        if (phase == 1) {
            expect(reference_webdav_put(config, "/conditional.txt",
                                        (const unsigned char *)"old",
                                        3u) == REFERENCE_WEBDAV_OK,
                   "seed CAS");
            expect(reference_webdav_read(config, "/conditional.txt", &body,
                                         &size, &entry) == REFERENCE_WEBDAV_OK,
                   "read CAS tag");
            free(body);
            snprintf(etag, sizeof(etag), "\"%s\"", entry.etag);
        }
        expect(pipe(gate) == 0, "create writer gate");
        for (i = 0; i < 8; ++i) {
            writers[i].config = config;
            writers[i].etag = phase ? etag : NULL;
            writers[i].gate = gate[0];
            writers[i].result = REFERENCE_WEBDAV_IO;
            expect(pthread_create(&threads[i], NULL, conditional_write,
                                  &writers[i]) == 0,
                   "start conditional writer");
        }
        expect(write(gate[1], "12345678", 8u) == 8,
               "release competing writers");
        winners = 0;
        for (i = 0; i < 8; ++i) {
            expect(pthread_join(threads[i], NULL) == 0, "join writer");
            if (writers[i].result == REFERENCE_WEBDAV_OK)
                ++winners;
            else
                expect(writers[i].result == REFERENCE_WEBDAV_PRECONDITION,
                       "loser rejects without mutation");
        }
        close(gate[0]);
        close(gate[1]);
        expect(winners == 1, "exactly one competing conditional write commits");
    }
    expect(reference_webdav_read(config, "/conditional.txt", &body, &size,
                                 &entry) == REFERENCE_WEBDAV_OK,
           "read winner");
    expect(size == 6u && memcmp(body, "winner", 6u) == 0,
           "winning body preserved");
    free(body);
    snprintf(etag, sizeof(etag), "\"%s\"", entry.etag);
    snprintf(weak, sizeof(weak), "W/%s", etag);
    expect(reference_webdav_put_conditional(
               config, "/conditional.txt", (const unsigned char *)"bad", 3u,
               weak, NULL) == REFERENCE_WEBDAV_PRECONDITION,
           "weak If-Match rejected");
    expect(reference_webdav_put_conditional(
               config, "/conditional.txt", (const unsigned char *)"bad", 3u,
               NULL, weak) == REFERENCE_WEBDAV_PRECONDITION,
           "weak If-None-Match matches");
    expect(reference_webdav_put_conditional(
               config, "/conditional.txt", (const unsigned char *)"bad", 3u,
               "\"x\",", NULL) == REFERENCE_WEBDAV_INVALID,
           "malformed list rejected");
    snprintf(list, sizeof(list), "\"other,tag\", %s", etag);
    expect(reference_webdav_put_conditional(config, "/conditional.txt",
                                            (const unsigned char *)"new", 3u,
                                            list, NULL) == REFERENCE_WEBDAV_OK,
           "quoted comma and matching tag list accepted");
    expect(reference_webdav_delete(config, "/conditional.txt") ==
               REFERENCE_WEBDAV_OK,
           "delete conditional fixture");
    expect(reference_webdav_put_conditional(config, "/conditional.txt",
                                            (const unsigned char *)"new", 3u,
                                            NULL, "*") == REFERENCE_WEBDAV_OK,
           "exclusive create replaces tombstone");
    expect(reference_webdav_delete(config, "/conditional.txt") ==
               REFERENCE_WEBDAV_OK,
           "clean conditional fixture");
    expect(pipe(gate) == 0, "create process writer gate");
    for (i = 0; i < 8; ++i) {
        children[i] = fork();
        expect(children[i] >= 0, "fork conditional writer");
        if (children[i] == 0) {
            conditional_writer writer;
            writer.config = config;
            writer.etag = NULL;
            writer.gate = gate[0];
            writer.result = REFERENCE_WEBDAV_IO;
            conditional_write(&writer);
            _exit(writer.result == REFERENCE_WEBDAV_OK             ? 0
                  : writer.result == REFERENCE_WEBDAV_PRECONDITION ? 1
                                                                   : 2);
        }
    }
    expect(write(gate[1], "12345678", 8u) == 8, "release process writers");
    winners = 0;
    for (i = 0; i < 8; ++i) {
        expect(waitpid(children[i], &child_status, 0) == children[i],
               "join process writer");
        expect(WIFEXITED(child_status) && WEXITSTATUS(child_status) <= 1,
               "process write result");
        if (WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0)
            ++winners;
    }
    close(gate[0]);
    close(gate[1]);
    expect(winners == 1, "exactly one competing process commits");
    expect(reference_webdav_delete(config, "/conditional.txt") ==
               REFERENCE_WEBDAV_OK,
           "clean process fixture");
}

static int listed(const char *path, reference_webdav_entry_kind kind,
                  size_t size, void *data)
{
    int *markers = data;
    if (!strcmp(path, "/marker")) {
        expect(kind == REFERENCE_WEBDAV_ENTRY_FILE && size == 2,
               "listing metadata");
        ++*markers;
    }
    return 1;
}
static int stop_listing(const char *path, reference_webdav_entry_kind kind,
                        size_t size, void *data)
{
    (void)path;
    (void)kind;
    (void)size;
    ++*(int *)data;
    return 0;
}
static void *standalone(void *data)
{
    reference_webdav_config config;
    reference_webdav_entry *entry = calloc(1, sizeof(*entry));
    unsigned char *bytes = NULL;
    size_t count = 0;
    char *root = data;
    char *direct = malloc(4096);
    int mode, markers, stopped;
    if (!entry || !direct) {
        ++failures;
        free(entry);
        free(direct);
        return NULL;
    }
    snprintf(direct, 4096, "%s/direct", root);
    expect(mkdir(direct, 0700) == 0, "direct root");
    for (mode = 0; mode < 2; ++mode) {
        reference_webdav_config_init(&config);
        config.cache_dir = root;
        config.site_id = mode ? "direct" : "managed";
        config.root_dir = mode ? direct : NULL;
        markers = 0;
        expect(reference_webdav_list(&config, "/", listed, &markers) ==
                   (mode ? REFERENCE_WEBDAV_OK : REFERENCE_WEBDAV_NOT_FOUND),
               "listing does not prepare a missing managed tree");
        test_delete_and_shallow_copy(&config);
        test_conditional_put(&config);
        if (mode)
            test_failed_move(&config);
        expect(reference_webdav_put(&config, "/marker", "ok", 2) ==
                   REFERENCE_WEBDAV_OK,
               "marker callback without HTTP worker");
        expect(reference_webdav_list(&config, "/marker", listed, &markers) ==
                   REFERENCE_WEBDAV_IO,
               "listing a file preserves baseline error");
        expect(reference_webdav_read(&config, "/marker", &bytes, &count,
                                     entry) == REFERENCE_WEBDAV_OK &&
                   count == 2 && !memcmp(bytes, "ok", 2) &&
                   strlen(entry->etag) == 64,
               "standalone read and unquoted ETag");
        free(bytes);
        markers = stopped = 0;
        expect(reference_webdav_list(&config, "/", listed, &markers) ==
                       REFERENCE_WEBDAV_OK &&
                   markers == 1,
               "standalone listing and marker visibility");
        expect(reference_webdav_list(&config, "/", stop_listing, &stopped) ==
                       REFERENCE_WEBDAV_OK &&
                   stopped == 1,
               "listing callback short circuit");
        expect(reference_webdav_put(&config, "/empty", NULL, 0) ==
                   REFERENCE_WEBDAV_OK,
               "empty standalone PUT");
        bytes = NULL;
        count = 1;
        expect(reference_webdav_read(&config, "/empty", &bytes, &count,
                                     entry) == REFERENCE_WEBDAV_OK &&
                   !bytes && !count && strlen(entry->etag) == 64,
               "empty materialized read");
        expect(reference_webdav_delete(&config, "/marker") ==
                   REFERENCE_WEBDAV_OK,
               "standalone marker deletion");
        expect(reference_webdav_lookup(&config, "/marker", entry) ==
                   (mode ? REFERENCE_WEBDAV_NOT_FOUND : REFERENCE_WEBDAV_OK),
               "managed tombstone versus direct removal");
        if (!mode)
            expect(entry->kind == REFERENCE_WEBDAV_ENTRY_TOMBSTONE,
                   "standalone tombstone entry");
    }
    free(entry);
    free(direct);
    return NULL;
}
int main(void)
{
    char name[] = "standalone-filesystem-XXXXXX";
    char *cwd, *root;
    pthread_t thread;
    char normalized[ROTMOS_FILESYSTEM_PATH_MAX + 1];
    expect(reference_webdav_path_normalize("//a///b", normalized) &&
               !strcmp(normalized, "/a/b"),
           "baseline normalization");
    if (!mkdtemp(name) || !(cwd = getcwd(NULL, 0)))
        return 1;
    root = malloc(strlen(cwd) + strlen(name) + 2);
    if (!root) {
        free(cwd);
        return 1;
    }
    sprintf(root, "%s/%s", cwd, name);
    free(cwd);
    if (pthread_create(&thread, NULL, standalone, root) ||
        pthread_join(thread, NULL))
        return 1;
    remove_tree(root);
    free(root);
    return failures ? 1 : 0;
}
