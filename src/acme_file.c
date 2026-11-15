#define _POSIX_C_SOURCE 200809L
#include "acme_internal.h"
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static int private_path(int parent, const char *name, int directory)
{
    struct stat st;
    int fd = openat(parent, name,
                    O_RDONLY | O_CLOEXEC | O_NOFOLLOW |
                        (directory ? O_DIRECTORY : O_NONBLOCK));
    if (fd < 0)
        return -1;
    if (fstat(fd, &st) != 0 || st.st_uid != geteuid() ||
        (st.st_mode & 077) != 0 ||
        (directory ? !S_ISDIR(st.st_mode) : !S_ISREG(st.st_mode))) {
        close(fd);
        errno = EACCES;
        return -1;
    }
    return fd;
}
static int sync_file(int parent, const char *name)
{
    int fd = private_path(parent, name, 0), result;
    if (fd < 0)
        return 0;
    result = fsync(fd);
    if (close(fd) != 0)
        result = -1;
    return result == 0;
}
static knurl_status file_state(knurl_server *server,
                               const knurl_process_context *process,
                               knurl_acme_event event, knurl_bytes domain,
                               knurl_bytes directory, void *data,
                               knurl_error *error)
{
    char path[4096], name[254];
    int root = -1, certificates = -1, site = -1, ok = 0;
    (void)server;
    (void)process;
    (void)data;
    if (event == KNURL_ACME_ERROR) {
        knurl_error_clear(error);
        return KNURL_OK;
    }
    if (directory.data == NULL || directory.length == 0 ||
        directory.length >= sizeof(path) ||
        memchr(directory.data, 0, directory.length))
        return knurl_fail(error, KNURL_INVALID, "acme_file",
                          "invalid private ACME state directory");
    memcpy(path, directory.data, directory.length);
    path[directory.length] = 0;
    root = private_path(AT_FDCWD, path, 1);
    if (root < 0)
        goto done;
    if (event == KNURL_ACME_RESTORE) {
        ok = 1;
        goto done;
    }
    if (event != KNURL_ACME_PERSIST || domain.data == NULL ||
        domain.length == 0 || domain.length >= sizeof(name) ||
        memchr(domain.data, 0, domain.length))
        goto done;
    memcpy(name, domain.data, domain.length);
    name[domain.length] = 0;
    if (!knurl_acme_domain_valid(name))
        goto done;
    certificates = private_path(root, "certificates", 1);
    if (certificates < 0)
        goto done;
    site = private_path(certificates, name, 1);
    if (site < 0)
        goto done;
    ok = sync_file(root, "account-key.pem") && sync_file(site, "key.pem") &&
         sync_file(site, "fullchain.pem") && fsync(site) == 0 &&
         fsync(certificates) == 0 && fsync(root) == 0;
done:
    if (site >= 0)
        close(site);
    if (certificates >= 0)
        close(certificates);
    if (root >= 0)
        close(root);
    if (!ok)
        return knurl_fail(error, KNURL_IO, "acme_file",
                          "cannot acknowledge durable private ACME state");
    knurl_error_clear(error);
    return KNURL_OK;
}
knurl_acme_state_fn knurl_acme_file_state(void) { return file_state; }
