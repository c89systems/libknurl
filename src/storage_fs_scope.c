#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "storage_fs_disk.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
struct scope_frame {
    struct scope_frame *parent;
    DIR *directory;
    char path[KNURL_FS_PATH_MAX + 1];
};
struct scope_walk {
    struct scope_frame *frame;
    knurl_fs_config content;
    char root[KNURL_FS_STORAGE_PATH_MAX];
    char path[KNURL_FS_PATH_MAX + 1], mapped[KNURL_FS_PATH_MAX + 1];
    knurl_fs_entry entry;
};
static struct scope_frame *frame_new(int fd, const char *path)
{
    struct scope_frame *frame = calloc(1, sizeof(*frame));
    if (frame == NULL) {
        close(fd);
        return NULL;
    }
    frame->directory = fdopendir(fd);
    if (frame->directory == NULL) {
        close(fd);
        free(frame);
        return NULL;
    }
    strcpy(frame->path, path);
    return frame;
}
static knurl_storage_code check(const knurl_fs_config *config, const char *path,
                                const char *source, const char *target,
                                char *mapped)
{
    int n;
    if (knurl_job_cancelled(config->job))
        return KNURL_STORAGE_IO;
    if (target == NULL)
        return config->scope(path, NULL, config->scope_data);
    n = snprintf(mapped, KNURL_FS_PATH_MAX + 1, "%s%s", target,
                 path + strlen(source));
    if (n < 0 || n > (int)KNURL_FS_PATH_MAX)
        return KNURL_STORAGE_LIMIT;
    return config->scope(path, mapped, config->scope_data);
}
static knurl_storage_code tree(const knurl_fs_config *config,
                               const char *source, const char *target,
                               int recursive)
{
    struct scope_walk *w;
    struct scope_frame *frame, *parent, *child;
    struct dirent *item;
    struct stat st;
    knurl_storage_code code;
    int fd, n;
    w = calloc(1, sizeof(*w));
    if (w == NULL)
        return KNURL_STORAGE_NOMEM;
    code = check(config, source, source, target, w->mapped);
    if (code != KNURL_STORAGE_OK || !recursive)
        goto done;
    code = knurl_fs_lookup(config, source, &w->entry);
    if (code == KNURL_STORAGE_NOT_FOUND ||
        (code == KNURL_STORAGE_OK &&
         w->entry.kind != KNURL_FS_ENTRY_COLLECTION)) {
        code = KNURL_STORAGE_OK;
        goto done;
    }
    if (code != KNURL_STORAGE_OK)
        goto done;
    w->content = *config;
    if (config->root_dir == NULL) {
        if (!knurl_fs_disk_path(config, "content", "/", w->root)) {
            code = KNURL_STORAGE_INVALID;
            goto done;
        }
        w->content.root_dir = w->root;
    }
    fd = knurl_fs_direct_open_existing(&w->content, source, 1, NULL);
    if (fd < 0) {
        code = KNURL_STORAGE_IO;
        goto done;
    }
    w->frame = frame_new(fd, source);
    if (w->frame == NULL) {
        code = KNURL_STORAGE_NOMEM;
        goto done;
    }
    while (w->frame != NULL) {
        frame = w->frame;
        if (knurl_job_cancelled(config->job)) {
            code = KNURL_STORAGE_IO;
            break;
        }
        errno = 0;
        item = readdir(frame->directory);
        if (item == NULL) {
            if (errno != 0) {
                code = KNURL_STORAGE_IO;
                break;
            }
            w->frame = frame->parent;
            closedir(frame->directory);
            free(frame);
            continue;
        }
        if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0)
            continue;
        n = snprintf(w->path, sizeof(w->path), "%s%s%s", frame->path,
                     strcmp(frame->path, "/") == 0 ? "" : "/", item->d_name);
        if (n < 0 || n > (int)KNURL_FS_PATH_MAX) {
            code = KNURL_STORAGE_LIMIT;
            break;
        }
        code = check(config, w->path, source, target, w->mapped);
        if (code != KNURL_STORAGE_OK)
            break;
        if (fstatat(dirfd(frame->directory), item->d_name, &st,
                    AT_SYMLINK_NOFOLLOW) != 0) {
            code = KNURL_STORAGE_IO;
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            fd = openat(dirfd(frame->directory), item->d_name,
                        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (fd < 0) {
                code = KNURL_STORAGE_IO;
                break;
            }
            child = frame_new(fd, w->path);
            if (child == NULL) {
                code = KNURL_STORAGE_NOMEM;
                break;
            }
            child->parent = frame;
            w->frame = child;
        }
    }
done:
    frame = w->frame;
    while (frame != NULL) {
        parent = frame->parent;
        closedir(frame->directory);
        free(frame);
        frame = parent;
    }
    free(w);
    return code;
}
knurl_storage_code knurl_fs_scope(const knurl_fs_config *config,
                                  const char *source, const char *target,
                                  int recursive, int overwrite)
{
    knurl_storage_code code;
    if (config->scope == NULL)
        return KNURL_STORAGE_OK;
    code = tree(config, source, target, recursive);
    if (code == KNURL_STORAGE_OK && target != NULL && overwrite)
        code = tree(config, target, NULL, 1);
    return code;
}
