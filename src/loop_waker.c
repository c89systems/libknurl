#include "loop_internal.h"

#include <stdlib.h>
#include <unistd.h>

rotmos_status rotmos_waker_new(rotmos_loop *loop, rotmos_waker_fn callback,
                               rotmos_retire_fn retired, void *userdata,
                               rotmos_waker **out, rotmos_error *error)
{
    rotmos_waker *waker;
    rotmos_status status;
    if (out != NULL)
        *out = NULL;
    if (callback == NULL || out == NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "waker_new",
                           "callback and output are required");
    waker = calloc(1, sizeof(*waker));
    if (waker == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "waker_new",
                           "cannot allocate waker");
    status = rotmos_loop_accept(loop, &waker->item, ROTMOS_LOOP_WAKER, retired,
                                userdata, error);
    if (status != ROTMOS_OK) {
        free(waker);
        return status;
    }
    waker->callback = callback;
    *out = waker;
    return ROTMOS_OK;
}

rotmos_status rotmos_waker_signal(rotmos_waker *waker, rotmos_error *error)
{
    rotmos_loop *loop;
    if (waker == NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "waker_signal",
                           "waker is required");
    loop = waker->item.loop;
    if (loop->process_id != (long)getpid())
        return rotmos_fail(error, ROTMOS_STATE, "waker_signal",
                           "waker belongs to another process");
    (void)pthread_mutex_lock(&loop->mutex);
    if (waker->item.cancelled || !loop->accepting) {
        (void)pthread_mutex_unlock(&loop->mutex);
        return rotmos_fail(error, ROTMOS_STATE, "waker_signal",
                           "waker is closing");
    }
    (void)pthread_mutex_unlock(&loop->mutex);
    rotmos_loop_ready(&waker->item);
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_status rotmos_waker_destroy(rotmos_waker *waker, rotmos_error *error)
{
    rotmos_status status;
    if (waker == NULL) {
        rotmos_error_clear(error);
        return ROTMOS_OK;
    }
    status = rotmos_loop_owner(waker->item.loop, error);
    if (status != ROTMOS_OK)
        return status;
    rotmos_loop_cancel(&waker->item);
    rotmos_error_clear(error);
    return ROTMOS_OK;
}
