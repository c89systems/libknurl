#ifndef ROTMOS_TEST_CHANNEL_BACKEND_H
#define ROTMOS_TEST_CHANNEL_BACKEND_H
#include <rotmos/common.h>
int rotmos_test_prefix(rotmos_request *request);
int rotmos_test_prefix_finished(void);
int rotmos_test_prefix_init(void);
void rotmos_test_prefix_cleanup(void);
int rotmos_test_prefix_signal(void);
int rotmos_test_prefix_wait(void);
#endif
