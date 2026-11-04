#ifndef ROTMOS_PROXY_CHECK_H
#define ROTMOS_PROXY_CHECK_H
#include <stdio.h>
#include <stdlib.h>
#define PROXY_CHECK(test)                                                      \
    do {                                                                       \
        if (!(test)) {                                                         \
            (void)fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #test);   \
            abort();                                                           \
        }                                                                      \
    } while (0)
#endif
