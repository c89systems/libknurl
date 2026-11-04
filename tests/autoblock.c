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
static rotmos_bytes ip(const char *text)
{
    rotmos_bytes bytes;
    bytes.data = text;
    bytes.length = strlen(text);
    return bytes;
}
int main(void)
{
    rotmos_server_config config;
    rotmos_server *server;
    rotmos_error error;
    rotmos_autoblock_status_rule status = {404, 2};
    rotmos_autoblock_event_rule event = {"named", 2};
    struct rotmos_autoblock_entry *entry;
    pid_t child;
    int result;
    rotmos_server_config_init_production_webserver(&config);
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
    CHECK(rotmos_server_new(&config, &server, &error) == ROTMOS_OK);
    CHECK(server->autoblock == NULL);
    CHECK(rotmos_autoblock_open(server, &error) == ROTMOS_OK);
    rotmos_autoblock_note(server, ip("192.0.2.1"), ROTMOS_OBSERVE_HTTP_STATUS,
                          404, NULL);
    CHECK(rotmos_autoblock_allowed(server, ip("192.0.2.1")));
    rotmos_autoblock_note(server, ip("192.0.2.1"), ROTMOS_OBSERVE_HTTP_STATUS,
                          404, NULL);
    CHECK(!rotmos_autoblock_allowed(server, ip("192.0.2.1")));
    rotmos_autoblock_note(server, ip("192.0.2.2"), ROTMOS_OBSERVE_APPLICATION,
                          0, "named");
    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        rotmos_autoblock_note(server, ip("192.0.2.2"),
                              ROTMOS_OBSERVE_APPLICATION, 0, "named");
        _exit(0);
    }
    CHECK(waitpid(child, &result, 0) == child && WIFEXITED(result) &&
          WEXITSTATUS(result) == 0);
    CHECK(!rotmos_autoblock_allowed(server, ip("192.0.2.2")));
    rotmos_autoblock_note(server, ip("192.0.2.3"), ROTMOS_OBSERVE_TCP_STALL, 0,
                          NULL);
    rotmos_autoblock_note(server, ip("192.0.2.3"), ROTMOS_OBSERVE_TCP_STALL, 0,
                          NULL);
    CHECK(rotmos_autoblock_allowed(server, ip("192.0.2.3")));
    entry = &server->autoblock->shared->entries[0];
    entry->until = entry->window = 0;
    rotmos_autoblock_note(server, ip("192.0.2.3"), ROTMOS_OBSERVE_TLS_FAILURE,
                          0, NULL);
    CHECK(rotmos_autoblock_allowed(server, ip("192.0.2.3")));
    rotmos_autoblock_note(server, ip("192.0.2.3"), ROTMOS_OBSERVE_TLS_FAILURE,
                          0, NULL);
    CHECK(!rotmos_autoblock_allowed(server, ip("192.0.2.3")));
    entry->window = 0;
    rotmos_autoblock_note(server, ip("192.0.2.3"), ROTMOS_OBSERVE_TLS_FAILURE,
                          0, NULL);
    CHECK(rotmos_autoblock_allowed(server, ip("192.0.2.3")));
    CHECK(entry->counters[1] == 1);
    server->config.autoblock.tcp_stall_threshold = UINT_MAX;
    server->config.autoblock.block_seconds = UINT_MAX;
    entry->counters[0] = UINT_MAX;
    rotmos_autoblock_note(server, ip("192.0.2.3"), ROTMOS_OBSERVE_TCP_STALL, 0,
                          NULL);
    CHECK(entry->counters[0] == UINT_MAX &&
          entry->until > (uint64_t)0xffffffffUL);
    child = fork();
    CHECK(child >= 0);
    if (child == 0)
        _exit(rotmos_autoblock_lock(server->autoblock) ? 0 : 1);
    CHECK(waitpid(child, &result, 0) == child && WIFEXITED(result) &&
          WEXITSTATUS(result) == 0);
#if defined(__linux__)
    CHECK(rotmos_autoblock_allowed(server, ip("192.0.2.3")));
#else
    CHECK(!rotmos_autoblock_allowed(server, ip("192.0.2.3")));
#endif
    rotmos_autoblock_close(server);
    CHECK(server->autoblock == NULL);
    CHECK(rotmos_autoblock_open(server, &error) == ROTMOS_OK);
    CHECK(rotmos_autoblock_allowed(server, ip("192.0.2.2")));
    rotmos_autoblock_close(server);
    CHECK(rotmos_server_destroy(server, &error) == ROTMOS_OK);
    return 0;
}
