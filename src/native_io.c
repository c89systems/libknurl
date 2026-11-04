#include "native_io.h"
#include <rotmos/request.h>
static void retired(void *data) { ((rotmos_loop *)data)->io_executor = NULL; }
rotmos_status rotmos_native_io_pool(rotmos_server *server,
                                    rotmos_executor **out, rotmos_error *error)
{
    rotmos_executor_config config;
    rotmos_loop *loop = rotmos_server_loop(server);
    rotmos_status status;
    if (loop == NULL)
        return rotmos_fail(error, ROTMOS_STATE, "native_io",
                           "native I/O requires an active HTTP worker");
    if (loop->io_executor == NULL) {
        rotmos_executor_config_init(&config);
        config.retired = retired;
        config.userdata = loop;
        status =
            rotmos_executor_new(server, &config, &loop->io_executor, error);
        if (status != ROTMOS_OK)
            return status;
    }
    *out = loop->io_executor;
    return ROTMOS_OK;
}
