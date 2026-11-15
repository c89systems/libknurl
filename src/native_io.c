#include "native_io.h"
#include <knurl/request.h>
static void retired(void *data)
{
    ((knurl_loop *)data)->impl->io_executor = NULL;
}
knurl_status knurl_native_io_pool(knurl_server *server, knurl_executor **out,
                                  knurl_error *error)
{
    knurl_executor_config config;
    knurl_loop *loop = knurl_server_loop(server);
    knurl_status status;
    if (loop == NULL)
        return knurl_fail(error, KNURL_STATE, "native_io",
                          "native I/O requires an active HTTP worker");
    if (loop->impl->io_executor == NULL) {
        knurl_executor_config_init(&config);
        config.retired = retired;
        config.userdata = loop;
        status = knurl_executor_new(server, &config, &loop->impl->io_executor,
                                    error);
        if (status != KNURL_OK)
            return status;
    }
    *out = loop->impl->io_executor;
    return KNURL_OK;
}
