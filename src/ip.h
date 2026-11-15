#ifndef KNURL_IP_H
#define KNURL_IP_H
#include <knurl/mmdb.h>
struct knurl_ip_network {
    knurl_mmdb_address address;
    unsigned int bits;
};
int knurl_ip_normalize(const knurl_mmdb_address *, knurl_mmdb_address *);
int knurl_ip_network_parse(const char *, struct knurl_ip_network *);
int knurl_ip_contains(const struct knurl_ip_network *,
                      const knurl_mmdb_address *);
int knurl_ip_hostname(const char *);
int knurl_ip_compare(const knurl_mmdb_address *, const knurl_mmdb_address *);
#endif
