#include "loop_internal.h"
#include <knurl/loop.h>

#include <stdlib.h>
#include <unistd.h>

knurl_status knurl_waker_new(knurl_loop *loop, knurl_waker_fn callback,
                             knurl_retire_fn retired, void *userdata,
                             knurl_waker **out, knurl_error *error)
{
    knurl_waker *waker;
    knurl_status status;
    if (out != NULL)
        *out = NULL;
    if (callback == NULL || out == NULL)
        return knurl_fail(error, KNURL_INVALID, "waker_new",
                          "callback and output are required");
    waker = calloc(1, sizeof(struct knurl_waker_impl));
    if (waker == NULL)
        return knurl_fail(error, KNURL_NOMEM, "waker_new",
                          "cannot allocate waker");
    knurl_waker_receiver_init(waker);
    status = knurl_loop_accept(loop, &waker->impl->item, waker,
                               KNURL_LOOP_WAKER, retired, userdata, error);
    if (status != KNURL_OK) {
        free(waker);
        return status;
    }
    waker->impl->callback = callback;
    *out = waker;
    return KNURL_OK;
}

knurl_status knurl_waker_signal(knurl_waker *waker, knurl_error *error)
{
    knurl_loop *loop;
    if (waker == NULL)
        return knurl_fail(error, KNURL_INVALID, "waker_signal",
                          "waker is required");
    loop = waker->impl->item.loop;
    if (loop->impl->process_id != (long)getpid())
        return knurl_fail(error, KNURL_STATE, "waker_signal",
                          "waker belongs to another process");
    (void)pthread_mutex_lock(&loop->impl->mutex);
    if (waker->impl->item.cancelled || !loop->impl->accepting) {
        (void)pthread_mutex_unlock(&loop->impl->mutex);
        return knurl_fail(error, KNURL_STATE, "waker_signal",
                          "waker is closing");
    }
    (void)pthread_mutex_unlock(&loop->impl->mutex);
    knurl_loop_ready(&waker->impl->item);
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_status knurl_waker_destroy(knurl_waker *waker, knurl_error *error)
{
    knurl_status status;
    if (waker == NULL) {
        knurl_error_clear(error);
        return KNURL_OK;
    }
    status = knurl_loop_owner(waker->impl->item.loop, error);
    if (status != KNURL_OK)
        return status;
    knurl_loop_cancel(&waker->impl->item);
    knurl_error_clear(error);
    return KNURL_OK;
}

void knurl_waker_receiver_init(knurl_waker *receiver)
{
    static const knurl_waker methods = {knurl_waker_signal, knurl_waker_destroy,
                                        NULL};
    *receiver = methods;
    receiver->impl = (struct knurl_waker_impl *)receiver;
}
