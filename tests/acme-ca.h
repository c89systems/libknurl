#ifndef ROTMOS_TEST_ACME_CA_H
#define ROTMOS_TEST_ACME_CA_H

#include <openssl/ssl.h>
#include <sys/types.h>

struct test_ca_report {
    int failed;
    int directories;
    int nonces;
    int resolves;
    int creates;
    int orders;
    int challenges;
    int finalized;
    int issued;
    int ports_ready;
    unsigned short ports[2];
};

struct test_ca {
    EVP_PKEY *account;
    EVP_PKEY *domain;
    EVP_PKEY *signing;
    X509 *root;
    char *account_pem;
    char *domain_pem;
    char *root_pem;
    char *exponent;
    char *modulus;
    char *issued_pem;
    char base[96];
    char token[32];
    char nonce[32];
    unsigned char digest[32];
    struct test_ca_report *report;
    SSL_CTX *context;
    int listener;
    pid_t pid;
    int mode;
    int registered;
    int authorized;
    int nonce_used;
};

struct test_ca *test_ca_new(const char *directory, int mode);
int test_ca_start(struct test_ca *ca);
void test_ca_free(struct test_ca *ca);
int test_ca_write(const char *directory, const char *leaf, const char *value);
int test_ca_remove(const char *directory);
int test_ca_client(struct test_ca *ca, unsigned short port, int serial,
                   int status, int challenge);

#endif
