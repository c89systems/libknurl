#ifndef ROTMOS_TEST_ACME_BACKEND_H
#define ROTMOS_TEST_ACME_BACKEND_H

#include <stddef.h>
int rotmos_test_acme_send(const void *data, size_t length, int challenge);

#endif
