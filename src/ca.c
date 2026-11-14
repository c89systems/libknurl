#define _POSIX_C_SOURCE 200809L
#include <knurl/tls.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

const char *knurl_tls_system_ca_file(void)
{
    static const char *const paths[] = {"/etc/ssl/certs/ca-certificates.crt",
                                        "/etc/pki/tls/certs/ca-bundle.crt",
                                        "/etc/ssl/ca-bundle.pem",
                                        "/etc/ssl/cert.pem"};
    const char *value;
    struct stat info;
    size_t i;
    value = getenv("CURL_CA_BUNDLE");
    if (value != NULL && *value != '\0')
        return value;
    value = getenv("SSL_CERT_FILE");
    if (value != NULL && *value != '\0')
        return value;
    value = getenv("SSL_CERT_DIR");
    if (value != NULL && *value != '\0')
        return NULL;
    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i)
        if (stat(paths[i], &info) == 0 && S_ISREG(info.st_mode) &&
            info.st_size > 0 && access(paths[i], R_OK) == 0)
            return paths[i];
    return NULL;
}
