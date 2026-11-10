#ifndef KNURL_REFERENCE_HOST_H
#define KNURL_REFERENCE_HOST_H
#include "vectis-route.h"
/* The host retains the existing runtime control framing, shared readiness,
 * auth-maintenance policy and outer fork/start/stop/wait supervision. */
struct reference_kore_host {
    struct reference_app *app;
    int control_fd;
    int *app_ready;
    void (*notify_ready)(struct reference_app *, int);
    reference_status (*control)(struct reference_app *, int, int *,
                                reference_error *);
    void (*auth_cleanup)(struct reference_app *);
    reference_status (*fork_exit)(struct reference_app *, const char *, void *,
                                  reference_error *);
    void *userdata;
    knurl_server_callbacks requests;
};
void reference_host_configure(struct reference_kore_host *,
                              knurl_server_config *);
reference_status reference_host_run(struct reference_kore_host *,
                                    reference_error *);
#endif
