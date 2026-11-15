#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#include <knurl/knurl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(x)                                                               \
    do {                                                                       \
        if (!(x)) {                                                            \
            fprintf(stderr, "acme file %d: %s\n", __LINE__, #x);               \
            return 1;                                                          \
        }                                                                      \
    } while (0)
static int write_file(const char *path)
{
    FILE *file = fopen(path, "w");
    if (file == NULL)
        return 0;
    if (fputs("fixture", file) < 0 || fclose(file) != 0)
        return 0;
    return chmod(path, 0600) == 0;
}
int main(void)
{
    char root[] = "acme-file-XXXXXX", path[256], base[128];
    knurl_bytes directory, domain;
    knurl_error error;
    knurl_acme_state_fn state = knurl_acme_file_state();
    CHECK(mkdtemp(root) != NULL);
    directory.data = root;
    directory.length = strlen(root);
    domain.data = "localhost";
    domain.length = 9;
    CHECK(state(NULL, NULL, KNURL_ACME_RESTORE, domain, directory, NULL,
                &error) == KNURL_OK);
    CHECK(state(NULL, NULL, KNURL_ACME_PERSIST, domain, directory, NULL,
                &error) == KNURL_IO);
    snprintf(path, sizeof(path), "%s/certificates", root);
    CHECK(mkdir(path, 0700) == 0);
    snprintf(base, sizeof(base), "%s/certificates/localhost", root);
    CHECK(mkdir(base, 0700) == 0);
    snprintf(path, sizeof(path), "%s/account-key.pem", root);
    CHECK(write_file(path));
    snprintf(path, sizeof(path), "%s/key.pem", base);
    CHECK(write_file(path));
    snprintf(path, sizeof(path), "%s/fullchain.pem", base);
    CHECK(write_file(path));
    CHECK(state(NULL, NULL, KNURL_ACME_PERSIST, domain, directory, NULL,
                &error) == KNURL_OK);
    CHECK(chmod(path, 0644) == 0);
    CHECK(state(NULL, NULL, KNURL_ACME_PERSIST, domain, directory, NULL,
                &error) == KNURL_IO);
    CHECK(unlink(path) == 0 && symlink("key.pem", path) == 0);
    CHECK(state(NULL, NULL, KNURL_ACME_PERSIST, domain, directory, NULL,
                &error) == KNURL_IO);
    CHECK(unlink(path) == 0);
    snprintf(path, sizeof(path), "%s/key.pem", base);
    CHECK(unlink(path) == 0);
    CHECK(rmdir(base) == 0);
    snprintf(path, sizeof(path), "%s/certificates", root);
    CHECK(rmdir(path) == 0);
    snprintf(path, sizeof(path), "%s/account-key.pem", root);
    CHECK(unlink(path) == 0);
    domain.data = "../escape";
    domain.length = 9;
    CHECK(state(NULL, NULL, KNURL_ACME_PERSIST, domain, directory, NULL,
                &error) == KNURL_IO);
    CHECK(chmod(root, 0755) == 0);
    CHECK(state(NULL, NULL, KNURL_ACME_RESTORE, domain, directory, NULL,
                &error) == KNURL_IO);
    CHECK(rmdir(root) == 0);
    return 0;
}
