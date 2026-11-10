#define _POSIX_C_SOURCE 200809L
#include "../src/autoblock_internal.h"
#include <sys/wait.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(t)                                                               \
    do {                                                                       \
        if (!(t)) {                                                            \
            fprintf(stderr, "autoblock %d: %s\n", __LINE__, #t);               \
            return 1;                                                          \
        }                                                                      \
    } while (0)
static knurl_bytes ip(const char *text)
{
    knurl_bytes bytes;
    bytes.data = text;
    bytes.length = strlen(text);
    return bytes;
}
int main(void)
{
    knurl_server_config config;
    knurl_server *server;
    knurl_error error;
    knurl_autoblock_status_rule status = {404, 2};
    knurl_autoblock_event_rule event = {"named", 2};
    struct knurl_autoblock_entry *entry;
    pid_t child;
    int result;
    knurl_server_config_init_production_webserver(&config);
    CHECK(config.autoblock.status_rule_count == 4 &&
          config.autoblock.status_rules[0].threshold == 10 &&
          config.autoblock.status_rules[2].threshold == 50 &&
          config.autoblock.tcp_stall_threshold == 20 &&
          config.limits.hard_body_bytes.low ==
              config.limits.default_body_bytes.low);
    config.autoblock.status_rules = &status;
    config.autoblock.status_rule_count = 1;
    config.autoblock.event_rules = &event;
    config.autoblock.event_rule_count = 1;
    config.autoblock.max_entries = 2;
    config.autoblock.window_seconds = 1;
    config.autoblock.tcp_stall_threshold = 2;
    config.autoblock.tls_failure_threshold = 2;
    CHECK(knurl_server_new(&config, &server, &error) == KNURL_OK);
    CHECK(server->autoblock == NULL);
    CHECK(knurl_autoblock_open(server, &error) == KNURL_OK);
    knurl_autoblock_note(server, ip("192.0.2.1"), KNURL_OBSERVE_HTTP_STATUS,
                         404, NULL);
    CHECK(knurl_autoblock_allowed(server, ip("192.0.2.1")));
    knurl_autoblock_note(server, ip("192.0.2.1"), KNURL_OBSERVE_HTTP_STATUS,
                         404, NULL);
    CHECK(!knurl_autoblock_allowed(server, ip("192.0.2.1")));
    knurl_autoblock_note(server, ip("192.0.2.2"), KNURL_OBSERVE_APPLICATION, 0,
                         "named");
    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        knurl_autoblock_note(server, ip("192.0.2.2"), KNURL_OBSERVE_APPLICATION,
                             0, "named");
        _exit(0);
    }
    CHECK(waitpid(child, &result, 0) == child && WIFEXITED(result) &&
          WEXITSTATUS(result) == 0);
    CHECK(!knurl_autoblock_allowed(server, ip("192.0.2.2")));
    knurl_autoblock_note(server, ip("192.0.2.3"), KNURL_OBSERVE_TCP_STALL, 0,
                         NULL);
    knurl_autoblock_note(server, ip("192.0.2.3"), KNURL_OBSERVE_TCP_STALL, 0,
                         NULL);
    CHECK(knurl_autoblock_allowed(server, ip("192.0.2.3")));
    entry = &server->autoblock->shared->entries[0];
    entry->until = entry->window = 0;
    knurl_autoblock_note(server, ip("192.0.2.3"), KNURL_OBSERVE_TLS_FAILURE, 0,
                         NULL);
    CHECK(knurl_autoblock_allowed(server, ip("192.0.2.3")));
    knurl_autoblock_note(server, ip("192.0.2.3"), KNURL_OBSERVE_TLS_FAILURE, 0,
                         NULL);
    CHECK(!knurl_autoblock_allowed(server, ip("192.0.2.3")));
    entry->window = 0;
    knurl_autoblock_note(server, ip("192.0.2.3"), KNURL_OBSERVE_TLS_FAILURE, 0,
                         NULL);
    CHECK(knurl_autoblock_allowed(server, ip("192.0.2.3")));
    CHECK(entry->counters[1] == 1);
    server->config.autoblock.tcp_stall_threshold = UINT_MAX;
    server->config.autoblock.block_seconds = UINT_MAX;
    entry->counters[0] = UINT_MAX;
    knurl_autoblock_note(server, ip("192.0.2.3"), KNURL_OBSERVE_TCP_STALL, 0,
                         NULL);
    CHECK(entry->counters[0] == UINT_MAX &&
          entry->until > (uint64_t)0xffffffffUL);
    child = fork();
    CHECK(child >= 0);
    if (child == 0)
        _exit(knurl_autoblock_lock(server->autoblock) ? 0 : 1);
    CHECK(waitpid(child, &result, 0) == child && WIFEXITED(result) &&
          WEXITSTATUS(result) == 0);
#if defined(__linux__)
    CHECK(knurl_autoblock_allowed(server, ip("192.0.2.3")));
#else
    CHECK(!knurl_autoblock_allowed(server, ip("192.0.2.3")));
#endif
    knurl_autoblock_close(server);
    CHECK(server->autoblock == NULL);
    CHECK(knurl_autoblock_open(server, &error) == KNURL_OK);
    CHECK(knurl_autoblock_allowed(server, ip("192.0.2.2")));
    knurl_autoblock_close(server);
    CHECK(knurl_server_destroy(server, &error) == KNURL_OK);
    return 0;
}
