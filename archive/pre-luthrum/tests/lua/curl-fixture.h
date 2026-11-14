#ifndef KNURL_TEST_CURL_FIXTURE_H
#define KNURL_TEST_CURL_FIXTURE_H
#include <lua.h>
void knurl_test_curl_register(lua_State *);
void knurl_test_curl_cleanup(void);
#endif
