#include "library.h"
#include "acme.h"
#include "acme-backend.h"

#include <stdlib.h>
#include <string.h>

/* Drive the real ACME-to-key-manager message boundary without a remote CA.
 * The runtime, persistence, distribution and TLS handshakes remain real. */
int knurl_test_acme_send(const void *data, size_t length, int challenge)
{
    struct kore_keyreq *request;
    size_t size;
    if (worker == NULL || worker->id != KORE_WORKER_ACME)
        return 0;
    size = offsetof(struct kore_keyreq, data) + length;
    request = calloc(1, size);
    if (request == NULL)
        return 0;
    (void)strcpy(request->domain, "localhost");
    request->data_len = length;
    memcpy(request->data, data, length);
    kore_msg_send(KORE_WORKER_KEYMGR,
                  challenge ? KORE_ACME_CHALLENGE_CERT : KORE_ACME_INSTALL_CERT,
                  request, size);
    free(request);
    return 1;
}
