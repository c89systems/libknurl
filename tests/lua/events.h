#ifndef ROTMOS_LUA_TEST_EVENTS_H
#define ROTMOS_LUA_TEST_EVENTS_H
#include <openssl/ssl.h>
#include <stddef.h>
int rotmos_lua_test_exchange(unsigned short, const char *, const char *,
                             const void *, size_t, int, const void *, size_t);
int rotmos_lua_test_events(unsigned short, unsigned short, SSL_CTX *, int);
#endif
