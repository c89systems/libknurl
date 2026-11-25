#ifndef KNURL_EXECUTOR_INTERNAL_H
#define KNURL_EXECUTOR_INTERNAL_H
#include "executor.h"
#include "loop_internal.h"
void knurl_executors_stopping(knurl_loop *);
knurl_status knurl_blocking_pool_get(knurl_server *, knurl_executor **,
                                     knurl_error *);
#endif
