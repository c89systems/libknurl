#ifndef ROTMOS_NATIVE_IO_H
#define ROTMOS_NATIVE_IO_H
#include "loop_internal.h"
#include "executor.h"
rotmos_status rotmos_native_io_pool(rotmos_server *, rotmos_executor **,
                                    rotmos_error *);
#endif
