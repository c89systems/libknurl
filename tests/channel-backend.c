#include "channel-backend.h"
#include <rotmos/kore.h>
#include "kore.h"
#include "http.h"

#include <sys/socket.h>
#include <string.h>
#include <poll.h>
#include <unistd.h>

static unsigned char payload[32768];
static int completions;
static int failed;
static int notification[2] = {-1, -1};

static int prefix_done(struct netbuf *buffer)
{
    if (buffer->s_off != buffer->b_len)
        failed = 1;
    ++completions;
    if (completions == 1)
        net_send_stream(buffer->owner, payload, sizeof(payload), prefix_done,
                        NULL);
    return KORE_RESULT_OK;
}

int rotmos_test_prefix(rotmos_request *request)
{
    struct http_request *native;
    int size;
    native = rotmos_kore_request(request);
    completions = failed = 0;
    size = 4096;
    if (setsockopt(native->owner->fd, SOL_SOCKET, SO_SNDBUF, &size,
                   sizeof(size)) != 0)
        return 0;
    memset(payload, 'p', sizeof(payload));
    net_send_stream(native->owner, payload, sizeof(payload), prefix_done, NULL);
    return !TAILQ_EMPTY(&native->owner->send_queue);
}

int rotmos_test_prefix_finished(void) { return completions == 2 && !failed; }

int rotmos_test_prefix_init(void) { return pipe(notification) == 0; }

void rotmos_test_prefix_cleanup(void)
{
    (void)close(notification[0]);
    (void)close(notification[1]);
}

int rotmos_test_prefix_signal(void)
{
    return write(notification[1], "x", 1) == 1;
}

int rotmos_test_prefix_wait(void)
{
    struct pollfd fd;
    char byte;
    fd.fd = notification[0];
    fd.events = POLLIN;
    fd.revents = 0;
    return poll(&fd, 1, 3000) == 1 && read(notification[0], &byte, 1) == 1 &&
           byte == 'x';
}
