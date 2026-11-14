#define _POSIX_C_SOURCE 200809L
#include <knurl/knurl.h>
#include <knurl/lonejson.h>
#include "acl-wire.h"
#include <sys/wait.h>
#include <sys/stat.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#define CHECK(t)                                                               \
    do {                                                                       \
        if (!(t)) {                                                            \
            fprintf(stderr, "ACL wire %d: %s\n", __LINE__, #t);                \
            return 1;                                                          \
        }                                                                      \
    } while (0)
struct fixture {
    pid_t child;
    int mode;
    char directory[256], dns_file[384];
};
static int unexpected;
static knurl_status keep_identity(knurl_request *request, char *buffer,
                                  size_t capacity, size_t *length, void *data,
                                  knurl_error *error)
{
    (void)request;
    (void)buffer;
    (void)capacity;
    (void)data;
    *length = 0;
    knurl_error_clear(error);
    return KNURL_OK;
}
struct record {
    char value[8];
};
static const lonejson_field fields[] = {LONEJSON_FIELD_STRING_FIXED_REQ(
    struct record, value, "value", LONEJSON_OVERFLOW_FAIL)};
LONEJSON_MAP_DEFINE(record_map, struct record, fields);
static void pause_ms(long milliseconds)
{
    struct timespec delay;
    delay.tv_sec = milliseconds / 1000;
    delay.tv_nsec = milliseconds % 1000 * 1000000;
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
    }
}
static knurl_status forbidden(knurl_request *r, knurl_reply *reply, void *data,
                              knurl_error *error)
{
    (void)r;
    (void)reply;
    (void)data;
    (void)error;
    ++unexpected;
    return KNURL_STATE;
}
static knurl_status forbidden_hook(knurl_request *r, knurl_reply *reply,
                                   int *finish, void *data, knurl_error *error)
{
    (void)finish;
    return forbidden(r, reply, data, error);
}
static knurl_status country_lookup(const knurl_mmdb_address *address,
                                   knurl_mmdb_country *country, void *data,
                                   knurl_error *error)
{
    (void)data;
    knurl_error_clear(error);
    if (address->bytes[0] == 3)
        return KNURL_EOF;
    if (address->bytes[0] == 4)
        return KNURL_IO;
    if (address->bytes[0] == 5) {
        memcpy(country->code, "us", 3);
        return KNURL_OK;
    }
    memcpy(country->code, address->bytes[0] == 1 ? "US" : "SE", 3);
    return KNURL_OK;
}
static knurl_status raw(knurl_request *r, knurl_request_plan *plan, void *data,
                        knurl_error *error)
{
    (void)r;
    (void)plan;
    (void)data;
    (void)error;
    ++unexpected;
    return KNURL_STATE;
}
static knurl_status json_handler(knurl_request *r,
                                 const lonejson_record_view *a,
                                 const lonejson_record_view *b, void *data,
                                 knurl_error *error)
{
    (void)a;
    (void)b;
    return forbidden(r, NULL, data, error);
}
static knurl_status typed_handler(knurl_request *r,
                                  const lonejson_record_view *a,
                                  knurl_reply *reply, void *data,
                                  knurl_error *error)
{
    (void)a;
    return forbidden(r, reply, data, error);
}
static knurl_status rest_handler(knurl_request *r, const void *a,
                                 knurl_reply *reply, void *data,
                                 knurl_error *error)
{
    (void)a;
    return forbidden(r, reply, data, error);
}
static knurl_status reader_handler(const knurl_upload_metadata *m,
                                   knurl_source *s, knurl_reply *reply,
                                   void *data, knurl_error *error)
{
    (void)m;
    (void)s;
    return forbidden(NULL, reply, data, error);
}
static knurl_status asset_open(const char *path, knurl_asset **out, void *data,
                               knurl_error *error)
{
    (void)path;
    (void)out;
    return forbidden(NULL, NULL, data, error);
}
static knurl_status upload_write(knurl_request *r, const void *bytes,
                                 size_t size, size_t *consumed, void *state,
                                 void *data, knurl_error *error)
{
    (void)bytes;
    (void)size;
    (void)consumed;
    (void)state;
    return forbidden(r, NULL, data, error);
}
static knurl_status upload_finish(knurl_request *r, knurl_reply *reply,
                                  void *state, void *data, knurl_error *error)
{
    (void)state;
    return forbidden(r, reply, data, error);
}
static knurl_status identity(knurl_request *r, knurl_reply *reply, void *data,
                             knurl_error *error)
{
    knurl_bytes ip = knurl_request_effective_address(r);
    (void)data;
    return knurl_reply_bytes(reply, 200, "text/plain", ip, error);
}
static knurl_status allowed(knurl_request *r, knurl_reply *reply, void *data,
                            knurl_error *error)
{
    (void)r;
    (void)data;
    return knurl_reply_text(reply, 200, "text/plain", "allowed", error);
}
static knurl_status stop(knurl_request *r, knurl_reply *reply, void *data,
                         knurl_error *error)
{
    (void)data;
    if (knurl_server_request_stop(knurl_request_server(r), error) != KNURL_OK ||
        unexpected)
        return KNURL_STATE;
    return knurl_reply_text(reply, 200, "text/plain", "stopped", error);
}
static int version_set(struct fixture *f, char version)
{
    FILE *file = fopen(f->dns_file, "wb");
    CHECK(file && fputc(version, file) == version && fclose(file) == 0);
    return 0;
}
static knurl_status resolve(const char *name, knurl_mmdb_address *out,
                            size_t capacity, size_t *count, void *data,
                            knurl_error *error)
{
    struct fixture *f = data;
    FILE *file;
    int version;
    (void)name;
    if (capacity < 1 || (file = fopen(f->dns_file, "rb")) == NULL)
        return KNURL_IO;
    version = fgetc(file);
    (void)fclose(file);
    if (version == '2')
        return KNURL_BACKEND;
    if (version == '3')
        pause_ms(150);
    *count = 1;
    return knurl_mmdb_address_parse(
        version == '0' || version == '3' ? "1.1.1.1" : "2.2.2.2", out, error);
}
static int wait_status(unsigned short port, const char *ip, int expected)
{
    int i, code;
    char headers[128];
    snprintf(headers, sizeof(headers), "X-Forwarded-For: %s\r\n", ip);
    for (i = 0; i < 150; ++i) {
        code = knurl_test_acl_exchange(port, "GET", "/dynamic", headers, NULL);
        if (code == expected)
            return 0;
        pause_ms(10);
    }
    fprintf(stderr, "dynamic ACL %s did not reach %d\n", ip, expected);
    return 1;
}
static int client(struct fixture *f, unsigned short port)
{
    const char *peer = "127.0.0.1";
    int mode = f->mode;
#define IP(headers, expected)                                                  \
    CHECK(knurl_test_acl_exchange(port, "GET", "/identity", headers,           \
                                  expected) == 200)
    IP("", peer);
    if (mode < 3 || mode >= 6) {
        IP("X-Forwarded-For: 1.2.3.4, 10.0.0.2\r\n", mode == 0   ? peer
                                                     : mode == 2 ? "10.0.0.2"
                                                                 : "1.2.3.4");
        IP("X-Forwarded-For: 6.6.6.6, 198.51.100.7\r\n",
           mode ? "198.51.100.7" : peer);
        IP("X-Forwarded-For: invalid\r\nX-Real-IP: 9.9.9.9\r\n", peer);
        IP("X-Forwarded-For: 1.1.1.1\r\nX-Forwarded-For: 2.2.2.2\r\n", peer);
        IP("X-Real-IP: 9.9.9.9\r\n", mode ? "9.9.9.9" : peer);
        IP("X-Forwarded-For: ::ffff:192.0.2.1\r\n", mode ? "192.0.2.1" : peer);
    } else if (mode == 3) {
        IP("X-Forwarded-For: 1.2.3.4\r\n", peer);
        IP("X-Chain: 1.2.3.4, 10.1.2.3\r\n", "1.2.3.4");
        IP("X-Chain: 1.2.3.4, 10.1.2.3, 10.2.3.4\r\n", peer);
        IP("X-Actual: 192.0.2.3\r\n", "192.0.2.3");
    } else if (mode == 4) {
        IP("Forwarded: for=198.51.100.1;proto=https, For=10.1.2.3\r\n",
           "198.51.100.1");
        IP("Forwarded: for=\"[2001:db8::1]:1234\"\r\n", "2001:db8::1");
        IP("Forwarded: for=\"192.0.2.3:5678\"\r\n", "192.0.2.3");
        IP("Forwarded: for=unknown\r\n", peer);
        IP("Forwarded: for=1.2.3.4;For=2.3.4.5\r\n", peer);
        IP("Forwarded: for=\"[::1]:999999\"\r\n", peer);
        IP("Forwarded: for=1.2.3.4\r\nForwarded: for=2.2.2.2\r\n", peer);
        IP("Forwarded: for=\"[2001:db8::1]\";host=\"escaped\\\"host\"\r\n",
           "2001:db8::1");
    } else {
        IP("X-Forwarded-For: 1.2.3.4\r\nX-Real-IP: 9.9.9.9\r\n", peer);
    }
    if (mode == 1) {
        static const char *const malformed[] = {
            "X-Forwarded-For: ,1.2.3.4\r\n",
            "X-Forwarded-For: 1.2.3.4,\r\n",
            "X-Forwarded-For: 1.2.3.4,,10.0.0.2\r\n",
            "X-Forwarded-For: bad,10.0.0.2\r\n",
            "X-Forwarded-For: 1.2.3.4:80\r\n",
            "X-Forwarded-For: fe80::1%lo0\r\n",
            "X-Real-IP: 1.2.3.4\r\nX-Real-IP: 2.3.4.5\r\n"};
        size_t i;
        IP("X-Forwarded-For: 1.2.3.4, 2001:db8::2\r\n", "1.2.3.4");
        for (i = 0; i < sizeof(malformed) / sizeof(malformed[0]); ++i) {
            IP(malformed[i], peer);
            CHECK(knurl_test_acl_exchange(port, "GET", "/effective",
                                          malformed[i], NULL) == 403);
        }
        CHECK(knurl_test_acl_exchange(
                  port, "GET", "/effective",
                  "X-Forwarded-For: malformed\r\nX-Real-IP: 1.2.3.4\r\n",
                  NULL) == 403);
        CHECK(knurl_test_acl_exchange(
                  port, "GET", "/effective",
                  "X-Forwarded-For: 1.2.3.4\r\nX-Forwarded-For: 2.3.4.5\r\n",
                  NULL) == 403);
        CHECK(knurl_test_acl_exchange(port, "GET", "/effective",
                                      "X-Real-IP: malformed\r\n", NULL) == 403);
        CHECK(knurl_test_acl_exchange(port, "GET", "/peer",
                                      "X-Forwarded-For: malformed\r\n",
                                      "allowed") == 200);
        CHECK(knurl_test_acl_blocked(port) == 0);
        CHECK(knurl_test_acl_exchange(port, "GET", "/raw-proxy%2Fpath", NULL,
                                      NULL) == 403);
        CHECK(knurl_test_acl_exchange(
                  port, "GET", "/proxy",
                  "Connection: Upgrade\r\nUpgrade: "
                  "websocket\r\nSec-WebSocket-Version: "
                  "13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n",
                  NULL) == 403);
        CHECK(knurl_test_acl_exchange(port, "GET", "/raw", NULL, NULL) == 403);
        CHECK(knurl_test_acl_exchange(port, "GET", "/allowed", NULL,
                                      "allowed") == 200);
        CHECK(knurl_test_acl_exchange(port, "GET", "/effective",
                                      "X-Forwarded-For: 1.2.3.4\r\n",
                                      NULL) == 403);
        CHECK(knurl_test_acl_exchange(port, "GET", "/peer",
                                      "X-Forwarded-For: 1.2.3.4\r\n",
                                      "allowed") == 200);
        CHECK(knurl_test_acl_exchange(port, "GET", "/country",
                                      "X-Forwarded-For: 1.1.1.1\r\n",
                                      "allowed") == 200);
        CHECK(knurl_test_acl_exchange(port, "GET", "/country",
                                      "X-Forwarded-For: 2.2.2.2\r\n",
                                      NULL) == 403);
        CHECK(knurl_test_acl_exchange(port, "GET", "/country",
                                      "X-Forwarded-For: 3.3.3.3\r\n",
                                      NULL) == 403);
        CHECK(knurl_test_acl_exchange(port, "GET", "/unknown",
                                      "X-Forwarded-For: 3.3.3.3\r\n",
                                      "allowed") == 200);
        CHECK(knurl_test_acl_exchange(port, "GET", "/known",
                                      "X-Forwarded-For: 3.3.3.3\r\n",
                                      NULL) == 403);
        CHECK(knurl_test_acl_exchange(port, "GET", "/known",
                                      "X-Forwarded-For: 2.2.2.2\r\n",
                                      "allowed") == 200);
        CHECK(knurl_test_acl_exchange(
                  port, "POST", "/unknown",
                  "X-Forwarded-For: 4.4.4.4\r\nContent-Length: "
                  "999999999\r\nExpect: 100-continue\r\n",
                  NULL) == 503);
        CHECK(knurl_test_acl_exchange(port, "GET", "/unknown",
                                      "X-Forwarded-For: 5.5.5.5\r\n",
                                      NULL) == 503);
        /* A per-request lookup error cannot poison later valid lookups. */
        CHECK(knurl_test_acl_exchange(port, "GET", "/unknown",
                                      "X-Forwarded-For: 3.3.3.3\r\n",
                                      "allowed") == 200);
    }
    if (mode == 4) {
        static const char *const malformed[] = {
            "Forwarded: proto=https\r\n",
            "Forwarded: for=1.2.3.4;proto=http;PROTO=https\r\n",
            "Forwarded: for=1.2.3.4;\r\n",
            "Forwarded: for=\"[2001:db8::1]\r\n",
            "Forwarded: for=\"[1.2.3.4]\"\r\n",
            "Forwarded: for=\"1.2.3.4:\"\r\n",
            "Forwarded: for=\"1.2.3.4:-1\"\r\n",
            "Forwarded: for=_hidden\r\n",
            "Forwarded: for=\"[fe80::1%lo0]\"\r\n",
            "Forwarded: for=1.2.3.4,\r\n"};
        size_t i;
        IP("Forwarded: for=\"[::ffff:192.0.2.1]\"\r\n", "192.0.2.1");
        for (i = 0; i < sizeof(malformed) / sizeof(malformed[0]); ++i) {
            IP(malformed[i], peer);
            CHECK(knurl_test_acl_exchange(port, "GET", "/effective",
                                          malformed[i], NULL) == 403);
        }
    }
    if (mode >= 6) {
        CHECK(wait_status(port, "1.1.1.1", 200) == 0);
        CHECK(version_set(f, '1') == 0 &&
              wait_status(port, "2.2.2.2", 200) == 0);
        CHECK(knurl_test_acl_exchange(port, "GET", "/dynamic",
                                      "X-Forwarded-For: 1.1.1.1\r\n",
                                      NULL) == 403);
        CHECK(version_set(f, '2') == 0 &&
              wait_status(port, "2.2.2.2", 503) == 0);
        CHECK(version_set(f, '1') == 0 &&
              wait_status(port, "2.2.2.2", 200) == 0);
        CHECK(version_set(f, '3') == 0 &&
              wait_status(port, "2.2.2.2", 503) == 0);
        CHECK(knurl_test_acl_exchange(
                  port, "POST", "/dynamic",
                  "X-Forwarded-For: 2.2.2.2\r\nContent-Length: "
                  "999999999\r\nExpect: 100-continue\r\n",
                  NULL) == 503);
        if (mode == 7) {
            /* Stop while native DNS work still owns the ACL. */
            CHECK(knurl_test_acl_exchange(port, "GET", "/stop", NULL,
                                          "stopped") == 200);
            return 0;
        }
        /* The late result for 1.1.1.1 must never publish after its deadline. */
        pause_ms(160);
        CHECK(knurl_test_acl_exchange(port, "GET", "/dynamic",
                                      "X-Forwarded-For: 1.1.1.1\r\n",
                                      NULL) == 503);
        CHECK(version_set(f, '1') == 0 &&
              wait_status(port, "2.2.2.2", 200) == 0);
    }
    CHECK(knurl_test_acl_exchange(port, "GET", "/stop", NULL, "stopped") ==
          200);
    return 0;
}
static knurl_status ready(knurl_server *server,
                          const knurl_process_context *process, void *data,
                          knurl_error *error)
{
    struct fixture *f = data;
    knurl_bound_listener state;
    (void)process;
    if (knurl_server_listener_at(server, 0, &state, error) != KNURL_OK)
        return KNURL_STATE;
    {
        const knurl_route *route;
        if (knurl_router_route_at(knurl_server_router(server), 0, &route,
                                  error) != KNURL_OK ||
            knurl_route_set_acl((knurl_route *)route, NULL, error) !=
                KNURL_STATE)
            return KNURL_STATE;
    }
    knurl_error_clear(error);
    if (knurl_server_set_serving(server, 1, error) != KNURL_OK)
        return error->status;
    f->child = fork();
    if (f->child < 0)
        return KNURL_IO;
    if (f->child == 0) {
        int failed;
        signal(SIGTERM, SIG_DFL);
        signal(SIGPIPE, SIG_IGN);
        failed = client(f, state.port);
        if (failed)
            (void)knurl_test_acl_exchange(state.port, "GET", "/stop", NULL,
                                          NULL);
        _exit(failed);
    }
    return KNURL_OK;
}
static knurl_acl *acl_new(knurl_acl_config *config)
{
    knurl_acl *acl;
    knurl_error error;
    if (knurl_acl_new(config, &acl, &error) != KNURL_OK) {
        fprintf(stderr, "ACL new: %s\n", error.message);
        abort();
    }
    return acl;
}
static int run(int mode)
{
    struct fixture f;
    knurl_server_config config;
    knurl_listener_config listener;
    knurl_server *server;
    knurl_router *router;
    knurl_route_config route;
    knurl_route_group_config group_config;
    knurl_route_group *group;
    knurl_acl_config acl_config;
    knurl_acl *deny, *permit, *peer, *dynamic_acl, *country_acl, *unknown_acl,
        *known_acl;
    knurl_mmdb_config country_config;
    knurl_mmdb *country_provider;
    knurl_proxy_config proxy;
    knurl_static_config file;
    knurl_upload_stream_config upload;
    knurl_upload_reader_config reader;
    knurl_upload_file_config upload_file;
    knurl_json_route_config json;
    knurl_json_typed_route_config typed;
    knurl_xml_route_config xml;
    knurl_rest_route_config rest;
    knurl_webdav_config dav;
    knurl_websocket_route_config ws;
    knurl_asset_provider_config assets;
    knurl_asset_provider *provider;
    lonejson_schema_view schema;
    lonejson_error native;
    lonejson *runtime;
    knurl_error error;
    knurl_run_result result;
    const char *trusted[] = {"127.0.0.0/8", "10.0.0.0/8", "2001:db8::/32"};
    const char *star[] = {"*"};
    const char *local[] = {"127.0.0.0/8"};
    const char *names[] = {"dynamic.test"};
    const char *countries[] = {"US"};
    int status;
    size_t i;
    const char *group_paths[] = {"/group", "/list", "/rest-list"};
    memset(&f, 0, sizeof(f));
    f.mode = mode;
    f.child = -1;
    CHECK(getcwd(f.directory, sizeof(f.directory) - 32) != NULL);
    strcat(f.directory, "/knurl-acl-XXXXXX");
    CHECK(mkdtemp(f.directory) != NULL);
    snprintf(f.dns_file, sizeof(f.dns_file), "%s/dns", f.directory);
    CHECK(version_set(&f, '0') == 0);
    knurl_server_config_init(&config);
    knurl_listener_config_init(&listener);
    listener.bind = "127.0.0.1";
    config.listeners = &listener;
    config.listener_count = 1;
    config.workers.count = 1;
    config.callbacks.on_parent_ready = ready;
    config.callbacks.userdata = &f;
    if (mode) {
        config.client_ip.trusted_proxies = mode == 2 ? star : trusted;
        config.client_ip.trusted_proxy_count = mode == 2 ? 1 : 3;
    }
    if (mode == 3) {
        config.client_ip.forwarded_header = "x-chain";
        config.client_ip.real_ip_header = "x-actual";
        config.client_ip.max_hops = 2;
    }
    if (mode == 4) {
        config.client_ip.forwarded_header = "forwarded";
        config.client_ip.forwarded_format = KNURL_FORWARDED_RFC7239;
    }
    if (mode == 5)
        config.client_ip.forwarded_header = config.client_ip.real_ip_header =
            "";
    if (mode == 1)
        config.client_ip.resolve = keep_identity;
    CHECK(knurl_server_new(&config, &server, &error) == KNURL_OK);
    router = knurl_server_router(server);
    knurl_acl_config_init(&acl_config);
    acl_config.policy = KNURL_ACL_DENY;
    deny = acl_new(&acl_config);
    acl_config.allow.addresses = local;
    acl_config.allow.address_count = 1;
    permit = acl_new(&acl_config);
    acl_config.use_peer = 1;
    peer = acl_new(&acl_config);
    acl_config.use_peer = 0;
    acl_config.allow.addresses = names;
    acl_config.dns_refresh_ms = 10;
    acl_config.dns_timeout_ms = 30;
    acl_config.resolve = resolve;
    acl_config.resolve_userdata = &f;
    dynamic_acl = acl_new(&acl_config);
    knurl_mmdb_config_init(&country_config);
    country_config.lookup = country_lookup;
    CHECK(knurl_mmdb_new(&country_config, &country_provider, &error) ==
          KNURL_OK);
    knurl_acl_config_init(&acl_config);
    acl_config.country_provider = country_provider;
    acl_config.policy = KNURL_ACL_DENY;
    acl_config.allow.countries = countries;
    acl_config.allow.country_count = 1;
    country_acl = acl_new(&acl_config);
    acl_config.allow.country_count = 0;
    acl_config.allow.unknown = 1;
    unknown_acl = acl_new(&acl_config);
    acl_config.allow.unknown = 0;
    acl_config.deny.unknown = 1;
    acl_config.policy = KNURL_ACL_ALLOW;
    known_acl = acl_new(&acl_config);
    knurl_route_config_init(&route);
    route.method = KNURL_HTTP_ANY;
    route.handler = forbidden;
    route.acl = deny;
    route.path = "/blocked";
    route.hooks.auth = route.hooks.before = route.hooks.after = forbidden_hook;
    CHECK(router->route(router, &route, NULL, &error) == KNURL_OK);
    /* A second match must never be tried after the selected route denies. */
    route.acl = NULL;
    route.path = "^/blocked$";
    route.path_kind = KNURL_PATH_REGEX;
    CHECK(router->route(router, &route, NULL, &error) == KNURL_OK);
    route.path_kind = KNURL_PATH_LITERAL;
    route.acl = deny;
    route.hooks.auth = route.hooks.before = route.hooks.after = NULL;
    route.handler = NULL;
    route.on_headers = raw;
    route.path = "/raw";
    CHECK(router->route(router, &route, NULL, &error) == KNURL_OK);
    route.on_headers = NULL;
    route.hooks.auth = route.hooks.before = route.hooks.after = NULL;
    route.handler = allowed;
    route.path = "/effective";
    route.acl = permit;
    CHECK(router->route(router, &route, NULL, &error) == KNURL_OK);
    route.path = "/peer";
    route.acl = peer;
    CHECK(router->route(router, &route, NULL, &error) == KNURL_OK);
    for (i = 0; i < 3; ++i) {
        knurl_route_group_config_init(&group_config);
        group_config.prefix = group_paths[i];
        group_config.acl = deny;
        group_config.hooks.auth = group_config.hooks.before =
            group_config.hooks.after = forbidden_hook;
        CHECK(router->group(router, &group_config, &group, &error) == KNURL_OK);
        route.path = "/open";
        route.acl = peer;
        CHECK(group->route(group, &route, NULL, &error) == KNURL_OK);
    }
    knurl_proxy_config_init(&proxy);
    proxy.path = "/proxy";
    proxy.target = "http://127.0.0.1:1";
    proxy.acl = deny;
    CHECK(router->proxy(router, &proxy, NULL, &error) == KNURL_OK);
    proxy.path = "^/raw-proxy.*$";
    proxy.path_kind = KNURL_PATH_REGEX;
    CHECK(router->proxy(router, &proxy, NULL, &error) == KNURL_OK);
    knurl_static_config_init(&file);
    file.path = "/static";
    file.file_path = "absent-acl-input";
    file.acl = deny;
    CHECK(router->static_file(router, &file, NULL, &error) == KNURL_OK);
    knurl_asset_provider_config_init(&assets);
    assets.open = asset_open;
    CHECK(knurl_asset_provider_new(&assets, &provider, &error) == KNURL_OK);
    file.path = "/assets";
    file.provider = provider;
    CHECK(router->static_assets(router, &file, NULL, &error) == KNURL_OK);
    knurl_upload_stream_config_init(&upload);
    upload.path = "/upload";
    upload.methods = KNURL_HTTP_METHODS_ALL;
    upload.write = upload_write;
    upload.finish = upload_finish;
    upload.acl = deny;
    CHECK(router->upload_stream(router, &upload, NULL, &error) == KNURL_OK);
    knurl_upload_reader_config_init(&reader);
    reader.path = "/reader";
    reader.methods = KNURL_HTTP_METHODS_ALL;
    reader.handler = reader_handler;
    reader.acl = deny;
    CHECK(router->upload_reader(router, &reader, NULL, &error) == KNURL_OK);
    knurl_upload_file_config_init(&upload_file);
    upload_file.path = "/upload-file";
    upload_file.file_path = "absent-acl-output";
    upload_file.methods = KNURL_HTTP_METHODS_ALL;
    upload_file.acl = deny;
    CHECK(router->upload_file(router, &upload_file, NULL, &error) == KNURL_OK);
    runtime = lonejson_new(NULL, &native);
    CHECK(runtime != NULL &&
          knurl_json_schema_from_map(runtime, &record_map, &schema, &error) ==
              KNURL_OK);
    knurl_json_route_config_init(&json);
    json.path = "/json";
    json.methods = KNURL_HTTP_METHODS_ALL;
    json.handler = json_handler;
    json.acl = deny;
    CHECK(router->json_route(router, &json, NULL, &error) == KNURL_OK);
    knurl_json_typed_route_config_init(&typed);
    typed.path = "/typed";
    typed.methods = KNURL_HTTP_METHODS_ALL;
    typed.handler = typed_handler;
    typed.acl = deny;
    CHECK(router->json_typed_route(router, &typed, NULL, &error) == KNURL_OK);
    knurl_xml_route_config_init(&xml);
    xml.path = "/xml";
    xml.methods = KNURL_HTTP_METHODS_ALL;
    xml.input = &schema;
    xml.handler = typed_handler;
    xml.acl = deny;
    CHECK(router->xml_route(router, &xml, NULL, &error) == KNURL_OK);
    knurl_rest_route_config_init(&rest);
    rest.path = "/rest";
    rest.methods = KNURL_HTTP_METHODS_ALL;
    rest.handler = rest_handler;
    rest.acl = deny;
    CHECK(router->rest_route(router, &rest, NULL, &error) == KNURL_OK);
    knurl_webdav_config_init(&dav);
    dav.path_prefix = "/dav";
    dav.auth_disabled = 1;
    dav.storage.root_dir = f.directory;
    dav.storage.cache_dir = f.directory;
    dav.storage.site_id = "acl";
    dav.acl = deny;
    status = router->webdav(router, &dav, NULL, &error);
    if (status != KNURL_OK)
        fprintf(stderr, "WebDAV declaration: %s (%d)\n", error.message, status);
    CHECK(status == KNURL_OK);
    knurl_websocket_route_config_init(&ws);
    ws.path = "/ws";
    ws.acl = deny;
    CHECK(router->websocket(router, &ws, NULL, &error) == KNURL_OK);
    knurl_route_config_init(&route);
    route.path = "/identity";
    route.handler = identity;
    CHECK(router->route(router, &route, NULL, &error) == KNURL_OK);
    route.path = "/allowed";
    route.handler = allowed;
    route.acl = peer;
    CHECK(router->route(router, &route, NULL, &error) == KNURL_OK);
    route.path = "/dynamic";
    route.acl = mode >= 6 ? dynamic_acl : NULL;
    CHECK(router->route(router, &route, NULL, &error) == KNURL_OK);
    route.path = "/country";
    route.acl = country_acl;
    CHECK(router->route(router, &route, NULL, &error) == KNURL_OK);
    route.path = "/unknown";
    route.acl = unknown_acl;
    CHECK(router->route(router, &route, NULL, &error) == KNURL_OK);
    route.path = "/known";
    route.acl = known_acl;
    CHECK(router->route(router, &route, NULL, &error) == KNURL_OK);
    route.path = "/stop";
    route.handler = stop;
    route.acl = NULL;
    CHECK(router->route(router, &route, NULL, &error) == KNURL_OK);
    CHECK(knurl_server_validate(server, &error) == KNURL_OK);
    deny->close(deny, NULL);
    permit->close(permit, NULL);
    peer->close(peer, NULL);
    dynamic_acl->close(dynamic_acl, NULL);
    country_acl->close(country_acl, NULL);
    unknown_acl->close(unknown_acl, NULL);
    known_acl->close(known_acl, NULL);
    CHECK(knurl_server_run(server, &result, &error) == KNURL_OK);
    CHECK(waitpid(f.child, &status, 0) == f.child && WIFEXITED(status) &&
          WEXITSTATUS(status) == 0);
    CHECK(knurl_server_destroy(server, &error) == KNURL_OK);
    CHECK(country_provider->close(country_provider, &error) == KNURL_OK);
    CHECK(knurl_asset_provider_close(provider, &error) == KNURL_OK);
    lonejson_free(runtime);
    CHECK(access("absent-acl-output", F_OK) != 0);
    CHECK(unlink(f.dns_file) == 0 && rmdir(f.directory) == 0);
    return 0;
}
int main(void)
{
    int mode;
    for (mode = 0; mode < 8; ++mode)
        CHECK(run(mode) == 0);
    return 0;
}
