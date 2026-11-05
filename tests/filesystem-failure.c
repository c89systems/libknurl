/* Vectis test_vectis_webdav_atomic.c's close/sync/descriptor-reuse invariants,
 * applied to the shared synchronous/native PUT implementation. */
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
static int fail_calloc, fail_malloc, fail_sync, fail_close, fail_write,
    replacement = -1;
static void *fault_calloc(size_t n, size_t size);
static void *fault_malloc(size_t size);
static int fault_sync(int fd);
static int fault_close(int fd);
static ssize_t fault_write(int fd, const void *bytes, size_t size);
#define calloc fault_calloc
#define malloc fault_malloc
#define fsync fault_sync
#define close fault_close
#define write fault_write
#include "../src/storage_filesystem.c"
#undef calloc
#undef malloc
#undef fsync
#undef close
#undef write
static ssize_t fault_write(int fd, const void *bytes, size_t size)
{
    if (fail_write) {
        fail_write = 0;
        errno = ENOSPC;
        return -1;
    }
    return write(fd, bytes, size);
}
static void *fault_calloc(size_t n, size_t size)
{
    if (fail_calloc) {
        fail_calloc = 0;
        return NULL;
    }
    return calloc(n, size);
}
static void *fault_malloc(size_t size)
{
    if (fail_malloc) {
        fail_malloc = 0;
        return NULL;
    }
    return malloc(size);
}
static int fault_sync(int fd)
{
    if (fail_sync) {
        fail_sync = 0;
        errno = EIO;
        return -1;
    }
    return fsync(fd);
}
static int fault_close(int fd)
{
    struct stat st;
    int status;
    if (!fail_close || fstat(fd, &st) || !S_ISREG(st.st_mode))
        return close(fd);
    fail_close = 0;
    status = close(fd);
    if (status)
        return status;
    replacement = open("/dev/null", O_RDONLY);
    if (replacement >= 0 && replacement != fd) {
        if (dup2(replacement, fd) < 0)
            return -1;
        close(replacement);
        replacement = fd;
    }
    errno = EIO;
    return -1;
}
#define CHECK(t)                                                               \
    do {                                                                       \
        if (!(t)) {                                                            \
            fprintf(stderr, "filesystem fault %d: %s\n", __LINE__, #t);        \
            return 1;                                                          \
        }                                                                      \
    } while (0)
static int remove_owned(const char *path)
{
    DIR *directory = opendir(path);
    struct dirent *entry;
    char child[4096];
    struct stat st;
    if (!directory)
        return unlink(path);
    while ((entry = readdir(directory)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (lstat(child, &st))
            return 1;
        if (S_ISDIR(st.st_mode)) {
            if (remove_owned(child))
                return 1;
        } else if (unlink(child))
            return 1;
    }
    closedir(directory);
    return rmdir(path);
}
int main(void)
{
    char relative[] = "filesystem-fault-XXXXXX";
    char *cwd, *root, *direct;
    rotmos_storage_filesystem_config config;
    rotmos_filesystem_entry *entry = calloc(1, sizeof(*entry));
    rotmos_storage_code code;
    unsigned char *body;
    size_t size;
    int mode;
    CHECK(entry && mkdtemp(relative) && (cwd = getcwd(NULL, 0)));
    root = malloc(strlen(cwd) + strlen(relative) + 2);
    direct = malloc(strlen(cwd) + strlen(relative) + 12);
    CHECK(root && direct);
    sprintf(root, "%s/%s", cwd, relative);
    free(cwd);
    sprintf(direct, "%s/direct", root);
    CHECK(!mkdir(direct, 0700));
    rotmos_storage_filesystem_config_init(&config);
    config.cache_dir = root;
    config.root_dir = direct;
    config.site_id = "fault";
    CHECK(rotmos_storage_filesystem_put(&config, "/file", "old", 3, NULL,
                                        NULL) == ROTMOS_STORAGE_OK);
    for (mode = 0; mode < 5; ++mode) {
        fail_calloc = mode == 0;
        fail_malloc = mode == 1;
        fail_sync = mode == 2;
        fail_close = mode == 3;
        fail_write = mode == 4;
        code = rotmos_storage_filesystem_put(&config, "/file", "new", 3, NULL,
                                             NULL);
        fail_calloc = fail_malloc = fail_sync = fail_close = fail_write = 0;
        if (code != (mode < 2 ? ROTMOS_STORAGE_NOMEM : ROTMOS_STORAGE_IO))
            fprintf(stderr, "fault mode %d returned %s\n", mode,
                    rotmos_storage_code_string(code));
        CHECK(code == (mode < 2 ? ROTMOS_STORAGE_NOMEM : ROTMOS_STORAGE_IO));
        if (replacement >= 0) {
            CHECK(fcntl(replacement, F_GETFD) != -1);
            CHECK(!close(replacement));
            replacement = -1;
        }
        CHECK(rotmos_storage_filesystem_read(&config, "/file", &body, &size,
                                             entry) == ROTMOS_STORAGE_OK);
        CHECK(size == 3 && !memcmp(body, "old", 3));
        free(body);
    }
    CHECK(rotmos_storage_filesystem_put(&config, "/file", "next", 4, NULL,
                                        NULL) == ROTMOS_STORAGE_OK);
    CHECK(rotmos_storage_filesystem_read(&config, "/file", &body, &size,
                                         entry) == ROTMOS_STORAGE_OK &&
          size == 4);
    free(body);
    free(entry);
    CHECK(!remove_owned(root));
    free(root);
    free(direct);
    return 0;
}
