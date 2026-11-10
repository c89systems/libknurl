#ifndef KNURL_TEST_ACME_BACKEND_H
#define KNURL_TEST_ACME_BACKEND_H

#include <stddef.h>
int knurl_test_acme_send(const void *data, size_t length, int challenge);

#endif
