#define _POSIX_C_SOURCE 200809L
#include <knurl/tls.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define CHECK(x)                                                               \
    do {                                                                       \
        if (!(x)) {                                                            \
            fprintf(stderr, "CA %d: %s\n", __LINE__, #x);                      \
            return 1;                                                          \
        }                                                                      \
    } while (0)
int main(void)
{
    const char *path;
    CHECK(unsetenv("CURL_CA_BUNDLE") == 0 && unsetenv("SSL_CERT_FILE") == 0 &&
          unsetenv("SSL_CERT_DIR") == 0);
    path = knurl_tls_system_ca_file();
    CHECK(path == NULL || *path == '/');
    CHECK(setenv("SSL_CERT_DIR", "/missing/explicit-directory", 1) == 0);
    CHECK(knurl_tls_system_ca_file() == NULL);
    CHECK(setenv("SSL_CERT_FILE", "/missing/explicit.pem", 1) == 0);
    CHECK(!strcmp(knurl_tls_system_ca_file(), "/missing/explicit.pem"));
    CHECK(setenv("CURL_CA_BUNDLE", "/missing/curl.pem", 1) == 0);
    CHECK(!strcmp(knurl_tls_system_ca_file(), "/missing/curl.pem"));
    CHECK(setenv("CURL_CA_BUNDLE", "", 1) == 0);
    CHECK(!strcmp(knurl_tls_system_ca_file(), "/missing/explicit.pem"));
    return 0;
}
