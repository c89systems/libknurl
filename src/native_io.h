#ifndef KNURL_NATIVE_IO_H
#define KNURL_NATIVE_IO_H
#include "loop_internal.h"
#include "executor.h"
knurl_status knurl_native_io_pool(knurl_server *, knurl_executor **,
                                  knurl_error *);
#endif
