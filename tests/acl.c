#define _POSIX_C_SOURCE 200809L
#include <knurl/knurl.h>
#include <assert.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
static void check(knurl_acl *acl, const char *peer, const char *effective,
                  int expected)
{
    knurl_mmdb_address p, e;
    knurl_error error;
    int allowed = -1;
    assert(knurl_mmdb_address_parse(peer, &p, &error) == KNURL_OK);
    if (effective)
        assert(knurl_mmdb_address_parse(effective, &e, &error) == KNURL_OK);
    assert(acl->check(acl, &p, effective ? &e : NULL, &allowed, &error) ==
           KNURL_OK);
    assert(allowed == expected);
}
static knurl_acl *create(knurl_acl_config *config)
{
    knurl_acl *acl;
    knurl_error error;
    assert(knurl_acl_new(config, &acl, &error) == KNURL_OK);
    assert(acl->refresh(acl, &error) == KNURL_OK);
    return acl;
}
static knurl_status country(const knurl_mmdb_address *address,
                            knurl_mmdb_country *out, void *userdata,
                            knurl_error *error)
{
    (void)userdata;
    if (address->bytes[0] == 3)
        return KNURL_EOF;
    if (address->bytes[0] == 4) {
        knurl_error_clear(error);
        return KNURL_IO;
    }
    memcpy(out->code, address->bytes[0] == 1 ? "US" : "SE", 3);
    return KNURL_OK;
}
struct dns {
    int calls, version, fail, invalid, oversized;
};
static knurl_status resolve(const char *name, knurl_mmdb_address *out,
                            size_t capacity, size_t *count, void *userdata,
                            knurl_error *error)
{
    struct dns *dns = userdata;
    ++dns->calls;
    assert(capacity == 64 &&
           (!strcmp(name, "one.test") || !strcmp(name, "two.test")));
    if (dns->fail && !strcmp(name, "two.test"))
        return KNURL_BACKEND;
    *count = dns->oversized ? capacity + 1 : 1;
    assert(knurl_mmdb_address_parse(dns->version ? "2.2.2.2" : "1.1.1.1", out,
                                    error) == KNURL_OK);
    if (dns->invalid)
        out->size = 7;
    return KNURL_OK;
}
static void wait_due(void)
{
    struct timespec delay;
    delay.tv_sec = 0;
    delay.tv_nsec = 4000000;
    assert(nanosleep(&delay, NULL) == 0);
}
static knurl_status inert_handler(knurl_request *request, knurl_reply *reply,
                                  void *data, knurl_error *error)
{
    (void)request;
    (void)reply;
    (void)data;
    (void)error;
    assert(0);
    return KNURL_STATE;
}
static int startup_calls;
static knurl_status initial_failure(const char *name, knurl_mmdb_address *out,
                                    size_t capacity, size_t *count, void *data,
                                    knurl_error *error)
{
    (void)name;
    (void)out;
    (void)capacity;
    (void)count;
    (void)data;
    (void)error;
    ++startup_calls;
    return KNURL_BACKEND;
}
static knurl_status never_ready(knurl_server *server,
                                const knurl_process_context *context,
                                void *data, knurl_error *error)
{
    (void)server;
    (void)context;
    (void)data;
    (void)error;
    assert(0);
    return KNURL_STATE;
}
static void lifecycle(const char *fixtures)
{
    knurl_acl_config config;
    knurl_acl *acl, *other;
    knurl_router *router;
    knurl_route_config route;
    knurl_route *registered;
    knurl_route_group_config group_config;
    knurl_route_group *group;
    knurl_server_config server_config;
    knurl_listener_config listener;
    knurl_server *server;
    knurl_error error;
    knurl_run_result result;
    knurl_mmdb_address address;
    int allowed;
    char path[1024];
    const char *codes[] = {"US"};
    const char *names[] = {"failure.test"};
    knurl_acl_config_init(&config);
    acl = create(&config);
    other = create(&config);
    assert(knurl_router_new(&router, &error) == KNURL_OK);
    knurl_route_config_init(&route);
    route.path = "/";
    route.handler = inert_handler;
    route.acl = acl;
    assert(router->route(router, &route, &registered, &error) == KNURL_OK);
    knurl_route_group_config_init(&group_config);
    group_config.prefix = "/group";
    group_config.acl = acl;
    assert(router->group(router, &group_config, &group, &error) == KNURL_OK);
    assert(acl->close(acl, &error) == KNURL_OK);
    assert(registered->set_acl(registered, other, &error) == KNURL_OK);
    assert(registered->declaration(registered)->acl == other);
    assert(group->set_acl(group, other, &error) == KNURL_OK);
    assert(other->close(other, &error) == KNURL_OK);
    assert(registered->set_acl(registered, NULL, &error) == KNURL_OK);
    assert(group->set_acl(group, NULL, &error) == KNURL_OK);
    assert(registered->declaration(registered)->acl == NULL);
    assert(router->destroy(router, &error) == KNURL_OK);
    knurl_server_config_init(&server_config);
    knurl_listener_config_init(&listener);
    server_config.listeners = &listener;
    server_config.listener_count = server_config.workers.count = 1;
    server_config.callbacks.on_parent_ready = never_ready;
    config.allow.addresses = names;
    config.allow.address_count = 1;
    config.resolve = initial_failure;
    assert(knurl_acl_new(&config, &acl, &error) == KNURL_OK &&
           startup_calls == 0);
    assert(knurl_server_new(&server_config, &server, &error) == KNURL_OK);
    route.acl = acl;
    assert(knurl_router_route(knurl_server_router(server), &route, NULL,
                              &error) == KNURL_OK);
    assert(knurl_server_validate(server, &error) == KNURL_OK &&
           startup_calls == 0);
    assert(knurl_server_run(server, &result, &error) == KNURL_BACKEND &&
           startup_calls == 1);
    assert(result.reason == KNURL_RUN_STARTUP_FAILURE);
    assert(knurl_server_destroy(server, &error) == KNURL_OK);
    assert(acl->close(acl, &error) == KNURL_OK);
    knurl_acl_config_init(&config);
    assert(snprintf(path, sizeof(path), "%s/valid-28-6.mmdb", fixtures) <
           (int)sizeof(path));
    config.country_database = path;
    config.allow.countries = codes;
    config.allow.country_count = 1;
    config.policy = KNURL_ACL_DENY;
    acl = create(&config);
    check(acl, "1.2.3.4", NULL, 1);
    check(acl, "2.3.4.5", NULL, 0);
    assert(acl->close(acl, &error) == KNURL_OK);
    assert(snprintf(path, sizeof(path), "%s/bad-code.mmdb", fixtures) <
           (int)sizeof(path));
    acl = create(&config);
    assert(knurl_mmdb_address_parse("1.2.3.4", &address, &error) == KNURL_OK);
    assert(acl->check(acl, &address, NULL, &allowed, &error) == KNURL_BACKEND &&
           !allowed);
    acl->close(acl, NULL);
    assert(snprintf(path, sizeof(path), "%s/no-such-database.mmdb", fixtures) <
           (int)sizeof(path));
    assert(knurl_acl_new(&config, &acl, &error) == KNURL_OK);
    assert(acl->refresh(acl, &error) == KNURL_IO);
    assert(acl->check(acl, &address, NULL, &allowed, &error) == KNURL_IO &&
           !allowed);
    acl->close(acl, NULL);
    server_config.client_ip.forwarded_header = "bad\r\nheader";
    assert(knurl_server_new(&server_config, &server, &error) == KNURL_INVALID &&
           !server);
    server_config.client_ip.forwarded_header = NULL;
    server_config.client_ip.max_hops = 65;
    assert(knurl_server_new(&server_config, &server, &error) == KNURL_INVALID &&
           !server);
}
static int many_calls;
static knurl_status many(const char *name, knurl_mmdb_address *out,
                         size_t capacity, size_t *count, void *data,
                         knurl_error *error)
{
    size_t i;
    (void)name;
    (void)data;
    (void)error;
    assert(capacity == 64);
    ++many_calls;
    for (i = 0; i < 64; ++i) {
        memset(&out[i], 0, sizeof(out[i]));
        out[i].size = i % 2 ? 16 : 4;
        if (out[i].size == 4) {
            out[i].bytes[0] = 100;
            out[i].bytes[2] = (unsigned char)i;
            out[i].bytes[3] = 1;
        } else {
            out[i].bytes[0] = 0x20;
            out[i].bytes[1] = 1;
            out[i].bytes[2] = 0x0d;
            out[i].bytes[3] = 0xb8;
            out[i].bytes[15] = (unsigned char)i;
        }
    }
    *count = 64;
    return KNURL_OK;
}
static void indexed(void)
{
    knurl_acl_config config;
    knurl_acl *acl;
    const char *name[] = {"bulk.test"}, *duplicate[] = {"BULK.TEST"};
    char fqdn[255];
    const char *absolute[1];
    absolute[0] = fqdn;
    knurl_acl_config_init(&config);
    config.policy = KNURL_ACL_DENY;
    config.allow.addresses = name;
    config.allow.address_count = 1;
    config.resolve = many;
    acl = create(&config);
    assert(many_calls == 1);
    check(acl, "100.0.0.1", NULL, 1);
    check(acl, "100.0.62.1", NULL, 1);
    check(acl, "100.0.64.1", NULL, 0);
    check(acl, "2001:db8::1", NULL, 1);
    check(acl, "2001:db8::3f", NULL, 1);
    check(acl, "2001:db8::40", NULL, 0);
    acl->close(acl, NULL);
    config.deny.addresses = duplicate;
    config.deny.address_count = 1;
    acl = create(&config);
    assert(many_calls == 2);
    check(acl, "100.0.0.1", NULL, 0);
    check(acl, "2001:db8::3f", NULL, 0);
    acl->close(acl, NULL);
    memset(fqdn, 'a', 254);
    fqdn[63] = fqdn[127] = fqdn[191] = fqdn[253] = '.';
    fqdn[254] = 0;
    config.deny.address_count = 0;
    config.allow.addresses = absolute;
    acl = create(&config);
    assert(many_calls == 3);
    check(acl, "100.0.62.1", NULL, 1);
    acl->close(acl, NULL);
}
/* Flip every bit independently, including host bits in noncanonical networks.
 * The expected membership follows the bit position, independently of Core's
 * byte/mask implementation. Exercise every IPv4/IPv6 prefix in both policies.
 */
static void prefix_boundaries(void)
{
    static const char *const bases[] = {
        "165.90.195.60", "2001:db8:a55a:3cc3:9696:5aa5:c33c:6996"};
    knurl_acl_config config;
    knurl_acl *acl;
    knurl_mmdb_address base, changed, other;
    knurl_error error;
    char rule[96];
    const char *rules[1];
    size_t family, policy;
    unsigned int prefix, bit, width;
    int accepted;
    rules[0] = rule;
    for (family = 0; family < 2; ++family) {
        assert(knurl_mmdb_address_parse(bases[family], &base, &error) ==
               KNURL_OK);
        assert(knurl_mmdb_address_parse(bases[1 - family], &other, &error) ==
               KNURL_OK);
        width = base.size * 8;
        for (prefix = 0; prefix <= width + 1; ++prefix) {
            if (prefix > width)
                assert(snprintf(rule, sizeof(rule), "%s", bases[family]) <
                       (int)sizeof(rule));
            else
                assert(snprintf(rule, sizeof(rule), "%s/%u", bases[family],
                                prefix) < (int)sizeof(rule));
            for (policy = 0; policy < 2; ++policy) {
                knurl_acl_config_init(&config);
                config.policy = policy ? KNURL_ACL_ALLOW : KNURL_ACL_DENY;
                if (policy) {
                    config.deny.addresses = rules;
                    config.deny.address_count = 1;
                } else {
                    config.allow.addresses = rules;
                    config.allow.address_count = 1;
                }
                acl = create(&config);
                assert(acl->check(acl, &base, NULL, &accepted, &error) ==
                       KNURL_OK);
                assert(accepted == !(int)policy);
                for (bit = 0; bit < width; ++bit) {
                    changed = base;
                    changed.bytes[bit / 8] ^=
                        (unsigned char)(128u >> (bit % 8));
                    accepted = -1;
                    assert(acl->check(acl, &changed, NULL, &accepted, &error) ==
                           KNURL_OK);
                    assert(accepted == (policy ? bit < prefix : bit >= prefix));
                }
                assert(acl->check(acl, &other, NULL, &accepted, &error) ==
                       KNURL_OK);
                assert(accepted == (int)policy);
                acl->close(acl, NULL);
            }
        }
    }
}
struct resolver_fault {
    knurl_status status;
    size_t count;
    int invalid, calls;
};
static knurl_status faulty_resolve(const char *name, knurl_mmdb_address *out,
                                   size_t capacity, size_t *count, void *data,
                                   knurl_error *error)
{
    struct resolver_fault *fault = data;
    assert(!strcmp(name, "fault.test") && capacity == 2);
    ++fault->calls;
    *count = fault->count;
    assert(knurl_mmdb_address_parse("192.0.2.1", &out[0], error) == KNURL_OK);
    assert(knurl_mmdb_address_parse("::ffff:192.0.2.1", &out[1], error) ==
           KNURL_OK);
    if (fault->invalid)
        out[0].size = 5;
    return fault->status;
}
static void resolver_failures(void)
{
    static const struct {
        knurl_status returned, expected;
        size_t count;
        int invalid;
    } cases[] = {{KNURL_EOF, KNURL_BACKEND, 1, 0},
                 {KNURL_AGAIN, KNURL_BACKEND, 1, 0},
                 {KNURL_IO, KNURL_IO, 1, 0},
                 {KNURL_TIMEOUT, KNURL_TIMEOUT, 1, 0},
                 {KNURL_CANCELLED, KNURL_CANCELLED, 1, 0},
                 {KNURL_OK, KNURL_BACKEND, 0, 0},
                 {KNURL_OK, KNURL_LIMIT, 3, 0},
                 {KNURL_OK, KNURL_BACKEND, 1, 1}};
    struct resolver_fault fault;
    knurl_acl_config config;
    knurl_acl *acl;
    knurl_mmdb_address address;
    knurl_error error;
    const char *names[] = {"fault.test"};
    size_t i;
    int accepted;
    assert(knurl_mmdb_address_parse("192.0.2.1", &address, &error) == KNURL_OK);
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        memset(&fault, 0, sizeof(fault));
        fault.status = cases[i].returned;
        fault.count = cases[i].count;
        fault.invalid = cases[i].invalid;
        knurl_acl_config_init(&config);
        config.policy = KNURL_ACL_DENY;
        config.allow.addresses = names;
        config.allow.address_count = 1;
        config.resolve = faulty_resolve;
        config.resolve_userdata = &fault;
        config.dns_max_addresses = 2;
        assert(knurl_acl_new(&config, &acl, &error) == KNURL_OK &&
               !fault.calls);
        assert(acl->refresh(acl, &error) == cases[i].expected);
        assert(error.status == cases[i].expected &&
               strstr(error.detail, "fault.test"));
        accepted = 1;
        assert(acl->check(acl, &address, NULL, &accepted, &error) ==
                   cases[i].expected &&
               !accepted);
        fault.status = KNURL_OK;
        fault.count = 2;
        fault.invalid = 0;
        assert(acl->refresh(acl, &error) == KNURL_OK && fault.calls == 2);
        check(acl, "192.0.2.1", NULL, 1);
        check(acl, "::ffff:192.0.2.1", NULL, 1);
        check(acl, "192.0.2.2", NULL, 0);
        acl->close(acl, NULL);
    }
}
static void country_combinations(void)
{
    knurl_mmdb_config provider;
    knurl_mmdb *db;
    knurl_acl_config config;
    knurl_acl *acl;
    knurl_mmdb_address address;
    knurl_error error;
    const char *addresses[] = {"1.0.0.0/8"};
    const char *countries[] = {"US"};
    unsigned int mask, policy, client;
    int allowed, expected, allow_match, deny_match, needs_lookup;
    knurl_status status;
    knurl_mmdb_config_init(&provider);
    provider.lookup = country;
    assert(knurl_mmdb_new(&provider, &db, &error) == KNURL_OK);
    /* Independently combine address, country and unknown alternatives on both
     * sides, under both fallback policies. Client 4 has a database I/O error.
     */
    for (mask = 0; mask < 64; ++mask)
        for (policy = 0; policy < 2; ++policy) {
            knurl_acl_config_init(&config);
            config.country_provider = db;
            config.policy = policy ? KNURL_ACL_DENY : KNURL_ACL_ALLOW;
            config.allow.addresses = config.deny.addresses = addresses;
            config.allow.countries = config.deny.countries = countries;
            config.allow.address_count = (mask & 1) != 0;
            config.allow.country_count = (mask & 2) != 0;
            config.deny.address_count = (mask & 4) != 0;
            config.deny.country_count = (mask & 8) != 0;
            config.allow.unknown = (mask & 16) != 0;
            config.deny.unknown = (mask & 32) != 0;
            acl = create(&config);
            needs_lookup = (mask & (2 | 8 | 16 | 32)) != 0;
            for (client = 1; client <= 4; ++client) {
                memset(&address, 0, sizeof(address));
                address.size = 4;
                address.bytes[0] = (unsigned char)client;
                allow_match = (client == 1 && (mask & (1 | 2))) ||
                              (client == 3 && (mask & 16));
                deny_match = (client == 1 && (mask & (4 | 8))) ||
                             (client == 3 && (mask & 32));
                expected = !deny_match && (allow_match || !policy);
                allowed = 1;
                status = acl->check(acl, &address, NULL, &allowed, &error);
                if (client == 4 && needs_lookup)
                    assert(status == KNURL_IO && !allowed);
                else
                    assert(status == KNURL_OK && allowed == expected);
            }
            acl->close(acl, NULL);
        }
    db->close(db, NULL);
}
int main(int argc, char **argv)
{
    knurl_acl_config config;
    knurl_acl *acl;
    knurl_error error;
    knurl_mmdb_address ip;
    knurl_mmdb *db;
    knurl_mmdb_config provider;
    struct dns dns;
    const char *allow[] = {"10.0.0.0/8", "2001:db8::/33",
                           "::ffff:192.0.2.0/120"};
    const char *deny[] = {"10.1.0.0/16", "2001:db8:7fff::/48"};
    const char *all[] = {"0.0.0.0/0", "::/0"};
    const char *names[] = {"one.test", "two.test"};
    const char *countries[] = {"US"};
    const char *bad[] = {"1.2.3.999", "1.2.3.4/33", "::/129",
                         "a..b",      "a-.test",    "-a.test",
                         "*",         "1.2.3.4:80", "123"};
    size_t i;
    int accepted, calls;
    assert(argc == 2);
    lifecycle(argv[1]);
    indexed();
    prefix_boundaries();
    resolver_failures();
    country_combinations();
    knurl_acl_config_init(&config);
    assert(config.policy == KNURL_ACL_ALLOW && !config.dns_refresh_ms &&
           !config.use_peer);
    acl = create(&config);
    check(acl, "1.2.3.4", NULL, 1);
    assert(acl->refresh(acl, &error) == KNURL_AGAIN);
    assert(acl->retain(acl, &error) == KNURL_OK);
    assert(acl->close(acl, &error) == KNURL_OK);
    check(acl, "::1", NULL, 1);
    acl->close(acl, NULL);
    config.policy = KNURL_ACL_DENY;
    config.allow.addresses = allow;
    config.allow.address_count = 3;
    config.deny.addresses = deny;
    config.deny.address_count = 2;
    acl = create(&config);
    check(acl, "10.2.0.1", NULL, 1);
    check(acl, "10.1.0.1", NULL, 0);
    check(acl, "11.0.0.1", NULL, 0);
    check(acl, "::ffff:192.0.2.255", NULL, 1);
    check(acl, "192.0.3.0", NULL, 0);
    check(acl, "2001:db8:1234::", NULL, 1);
    check(acl, "2001:db8:8000::", NULL, 0);
    check(acl, "2001:db8:7fff::1", NULL, 0);
    check(acl, "1.1.1.1", "10.2.0.1", 1);
    acl->close(acl, NULL);
    config.use_peer = 1;
    acl = create(&config);
    check(acl, "1.1.1.1", "10.2.0.1", 0);
    acl->close(acl, NULL);
    config.use_peer = 0;
    config.allow.addresses = all;
    config.allow.address_count = 2;
    acl = create(&config);
    check(acl, "255.255.255.255", NULL, 1);
    check(acl, "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff", NULL, 1);
    acl->close(acl, NULL);
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        config.allow.addresses = &bad[i];
        config.allow.address_count = 1;
        assert(knurl_acl_new(&config, &acl, &error) == KNURL_INVALID &&
               acl == NULL);
    }
    knurl_acl_config_init(&config);
    memset(&dns, 0, sizeof(dns));
    config.allow.addresses = names;
    config.allow.address_count = 2;
    config.policy = KNURL_ACL_DENY;
    config.resolve = resolve;
    config.resolve_userdata = &dns;
    assert(knurl_acl_new(&config, &acl, &error) == KNURL_OK && dns.calls == 0);
    assert(knurl_mmdb_address_parse("1.1.1.1", &ip, &error) == KNURL_OK);
    assert(acl->check(acl, &ip, NULL, &accepted, &error) == KNURL_STATE &&
           !accepted);
    assert(acl->refresh(acl, &error) == KNURL_OK && dns.calls == 2);
    check(acl, "1.1.1.1", NULL, 1);
    dns.version = 1;
    assert(acl->refresh(acl, &error) == KNURL_AGAIN && dns.calls == 2);
    check(acl, "1.1.1.1", NULL, 1);
    check(acl, "2.2.2.2", NULL, 0);
    acl->close(acl, NULL);
    config.dns_refresh_ms = 1;
    dns.version = 0;
    acl = create(&config);
    calls = dns.calls;
    for (i = 0; i < 100; ++i)
        check(acl, "1.1.1.1", NULL, 1);
    assert(dns.calls == calls);
    wait_due();
    dns.version = 1;
    dns.fail = 1;
    assert(acl->refresh(acl, &error) == KNURL_BACKEND);
    assert(strstr(error.detail, "two.test") != NULL);
    assert(acl->check(acl, &ip, NULL, &accepted, &error) == KNURL_BACKEND &&
           !accepted);
    wait_due();
    dns.fail = 0;
    assert(acl->refresh(acl, &error) == KNURL_OK);
    check(acl, "1.1.1.1", NULL, 0);
    check(acl, "2.2.2.2", NULL, 1);
    wait_due();
    dns.oversized = 1;
    assert(acl->refresh(acl, &error) == KNURL_LIMIT);
    dns.oversized = 0;
    wait_due();
    dns.invalid = 1;
    assert(acl->refresh(acl, &error) == KNURL_BACKEND);
    acl->close(acl, NULL);
    knurl_mmdb_config_init(&provider);
    provider.lookup = country;
    assert(knurl_mmdb_new(&provider, &db, &error) == KNURL_OK);
    knurl_acl_config_init(&config);
    config.country_provider = db;
    config.allow.countries = countries;
    config.allow.country_count = 1;
    config.policy = KNURL_ACL_DENY;
    acl = create(&config);
    check(acl, "1.1.1.1", NULL, 1);
    check(acl, "2.2.2.2", NULL, 0);
    check(acl, "3.3.3.3", NULL, 0);
    assert(knurl_mmdb_address_parse("4.4.4.4", &ip, &error) == KNURL_OK);
    assert(acl->check(acl, &ip, NULL, &accepted, &error) == KNURL_IO &&
           !accepted);
    acl->close(acl, NULL);
    config.allow.unknown = 1;
    config.deny.countries = countries;
    config.deny.country_count = 1;
    acl = create(&config);
    check(acl, "1.1.1.1", NULL, 0);
    check(acl, "3.3.3.3", NULL, 1);
    acl->close(acl, NULL);
    config.allow.country_count = config.deny.country_count = 0;
    config.policy = KNURL_ACL_ALLOW;
    config.deny.unknown = 1;
    acl = create(&config);
    check(acl, "1.1.1.1", NULL, 1);
    check(acl, "3.3.3.3", NULL, 0);
    acl->close(acl, NULL);
    db->close(db, NULL);
    assert(knurl_acl_close(NULL, NULL) == KNURL_OK);
    puts("ACL address, DNS, country and ownership checks passed");
    return 0;
}
