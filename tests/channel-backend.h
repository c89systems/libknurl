#ifndef KNURL_TEST_CHANNEL_BACKEND_H
#define KNURL_TEST_CHANNEL_BACKEND_H
#include <knurl/common.h>
int knurl_test_prefix(knurl_request *request);
int knurl_test_prefix_finished(void);
int knurl_test_prefix_init(void);
void knurl_test_prefix_cleanup(void);
int knurl_test_prefix_signal(void);
int knurl_test_prefix_wait(void);
#endif
