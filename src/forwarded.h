#ifndef KNURL_FORWARDED_H
#define KNURL_FORWARDED_H
#include "ip.h"
#include <knurl/server.h>
struct knurl_forward_chain {
    knurl_mmdb_address hops[64];
    size_t count;
};
int knurl_forwarded_parse(const knurl_client_ip_config *, knurl_bytes,
                          struct knurl_forward_chain *);
#endif
