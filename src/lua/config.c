#include "binding.h"
/* These descriptors map only public C configuration fields. Initial values and
 * normalization remain in librotmos; the binding checks Lua representation. */
enum field_kind {
    SIZE_FIELD,
    UINT_FIELD,
    LONG_FIELD,
    BOOL_FIELD,
    WIDE_FIELD,
    TEXT_FIELD,
    ULONG_FIELD,
    DEADLINE_FIELD,
    DEATH_FIELD,
    MIME_FIELD
};
struct setting {
    const char *name;
    size_t offset;
    enum field_kind kind;
};
#define F(t, n, k) {#n, offsetof(t, n), k##_FIELD}
#define N(a) (sizeof(a) / sizeof((a)[0]))
static const struct setting workers[] = {
    F(rotmos_worker_config, count, SIZE),
    F(rotmos_worker_config, accept_threshold, SIZE),
    F(rotmos_worker_config, rlimit_nofiles, SIZE),
    F(rotmos_worker_config, shutdown_timeout_ms, LONG),
    F(rotmos_worker_config, affinity_disabled, BOOL),
    F(rotmos_worker_config, death_policy, DEATH)};
static const struct setting io[] = {
    F(rotmos_io_config, chunk_bytes, SIZE),
    F(rotmos_io_config, input_queue_bytes, SIZE),
    F(rotmos_io_config, output_queue_bytes, SIZE)};
static const struct setting limits[] = {
    F(rotmos_limit_config, max_connections, SIZE),
    F(rotmos_limit_config, request_limit, SIZE),
    F(rotmos_limit_config, max_request_header_bytes, SIZE),
    F(rotmos_limit_config, max_response_header_bytes, SIZE),
    F(rotmos_limit_config, websocket_max_frame_bytes, SIZE),
    F(rotmos_limit_config, default_body_bytes, WIDE),
    F(rotmos_limit_config, hard_body_bytes, WIDE),
    F(rotmos_limit_config, body_min_rate_bytes_per_second, WIDE),
    F(rotmos_limit_config, body_spool_directory, TEXT),
    F(rotmos_limit_config, body_min_rate_disabled, BOOL)};
static const struct setting http[] = {
    F(rotmos_http_config, mime_types, MIME),
    F(rotmos_http_config, keepalive_disabled, BOOL),
    F(rotmos_http_config, keepalive_max_requests, UINT),
    F(rotmos_http_config, socket_backlog, UINT),
    F(rotmos_http_config, request_process_budget_ms, UINT),
    F(rotmos_http_config, backend_quiet, BOOL),
    F(rotmos_http_config, server_header, TEXT),
    F(rotmos_http_config, access_log_path, TEXT),
    F(rotmos_http_config, hsts_max_age_seconds, ULONG),
    F(rotmos_http_config, backend_curl_timeout_seconds, ULONG),
    F(rotmos_http_config, backend_curl_recv_max_bytes, WIDE),
    F(rotmos_http_config, pretty_error_pages, BOOL)};
static const struct setting timeouts[] = {
    F(rotmos_timeout_config, startup, DEADLINE),
    F(rotmos_timeout_config, request_header, DEADLINE),
    F(rotmos_timeout_config, request_body_idle, DEADLINE),
    F(rotmos_timeout_config, response_write_idle, DEADLINE),
    F(rotmos_timeout_config, body_min_rate_grace, DEADLINE),
    F(rotmos_timeout_config, connection_idle, DEADLINE),
    F(rotmos_timeout_config, keepalive, DEADLINE),
    F(rotmos_timeout_config, websocket_idle, DEADLINE),
    F(rotmos_timeout_config, deferred_response, DEADLINE)};
static const struct setting deadline[] = {
    F(rotmos_deadline, milliseconds, LONG), F(rotmos_deadline, disabled, BOOL)};
static const struct setting proxy[] = {
    F(rotmos_proxy_resource_config, active_exchanges, SIZE),
    F(rotmos_proxy_resource_config, idle_connections, SIZE)};
static const struct setting blocking[] = {
    F(rotmos_autoblock_config, enabled, BOOL),
    F(rotmos_autoblock_config, window_seconds, UINT),
    F(rotmos_autoblock_config, block_seconds, UINT),
    F(rotmos_autoblock_config, max_entries, SIZE),
    F(rotmos_autoblock_config, tcp_stall_threshold, UINT),
    F(rotmos_autoblock_config, tls_failure_threshold, UINT)};
static const struct setting status_rule[] = {
    F(rotmos_autoblock_status_rule, status, UINT),
    F(rotmos_autoblock_status_rule, threshold, UINT)};
static const struct setting event_rule[] = {
    F(rotmos_autoblock_event_rule, name, TEXT),
    F(rotmos_autoblock_event_rule, threshold, UINT)};
static int settings(lua_State *L, int index, void *config,
                    const struct setting *fields, size_t count, int anchor,
                    rotmos_error *error)
{
    size_t i, n;
    unsigned char *base = config;
    int ok;
    const char *name;
    index = lua_absindex(L, index);
    lua_pushnil(L);
    while (lua_next(L, index)) {
        name = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : NULL;
        for (i = 0; name != NULL && i < count; ++i)
            if (!strcmp(name, fields[i].name))
                break;
        lua_pop(L, 1);
        if (name == NULL || i == count) {
            rl_fail(error, ROTMOS_INVALID, "unknown configuration field");
            if (name != NULL)
                snprintf(error->detail, sizeof(error->detail), "%s", name);
            lua_pop(L, 1);
            return 0;
        }
    }
    for (i = 0; i < count; ++i) {
        void *out = base + fields[i].offset;
        rl_field(L, index, fields[i].name);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            continue;
        }
        ok = 1;
        switch (fields[i].kind) {
        case MIME_FIELD:
            ok = rl_mime_types(L, -1, (const rotmos_mime_type **)out,
                               &((rotmos_http_config *)config)->mime_type_count,
                               error);
            if (ok)
                lua_setfield(L, anchor, fields[i].name);
            break;
        case WIDE_FIELD:
            ok = rl_u64(L, -1, out);
            break;
        case TEXT_FIELD: {
            const char *text;
            size_t length;
            ok = lua_type(L, -1) == LUA_TSTRING;
            if (ok) {
                text = lua_tolstring(L, -1, &length);
                ok = memchr(text, 0, length) == NULL;
                if (ok)
                    *(const char **)out = text;
            }
            break;
        }
        case BOOL_FIELD:
            ok = lua_isboolean(L, -1);
            if (ok)
                *(int *)out = lua_toboolean(L, -1);
            break;
        case DEADLINE_FIELD:
            ((rotmos_deadline *)out)->disabled = 0;
            if (lua_istable(L, -1))
                ok = settings(L, -1, out, deadline, N(deadline), anchor, error);
            else {
                ok = rl_size(L, -1, &n) && n <= (size_t)LONG_MAX;
                if (ok)
                    ((rotmos_deadline *)out)->milliseconds = (long)n;
            }
            break;
        case DEATH_FIELD:
            name = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : NULL;
            ok = name != NULL &&
                 (!strcmp(name, "restart") || !strcmp(name, "terminate"));
            if (ok)
                *(rotmos_worker_death_policy *)out =
                    !strcmp(name, "restart") ? ROTMOS_WORKER_RESTART
                                             : ROTMOS_WORKER_TERMINATE;
            break;
        default:
            ok = rl_size(L, -1, &n);
            if (!ok)
                break;
            if (fields[i].kind == SIZE_FIELD)
                *(size_t *)out = n;
            else if (fields[i].kind == UINT_FIELD) {
                ok = n <= UINT_MAX;
                if (ok)
                    *(unsigned int *)out = (unsigned int)n;
            } else if (fields[i].kind == ULONG_FIELD)
                *(unsigned long *)out = (unsigned long)n;
            else {
                ok = n <= (size_t)LONG_MAX;
                if (ok)
                    *(long *)out = (long)n;
            }
            break;
        }
        lua_pop(L, 1);
        if (!ok) {
            if (error->status == ROTMOS_OK)
                rl_fail(error, ROTMOS_INVALID,
                        "invalid configuration type or integer range");
            snprintf(error->detail, sizeof(error->detail), "%s",
                     fields[i].name);
            return 0;
        }
    }
    return 1;
}
/* Native declaration arrays are Lua-owned scratch. The anchor table remains on
 * the constructor stack until public server_new has copied every declaration.
 * An allocation error before that call has no native owner to unwind. */
struct scratch {
    int table;
    lua_Integer next;
};
static void *array(lua_State *L, struct scratch *a, size_t count, size_t width,
                   rotmos_error *error)
{
    void *out;
    if (count == 0)
        return NULL;
    if (count > (size_t)-1 / width) {
        rl_fail(error, ROTMOS_OVERFLOW, "configuration array is too large");
        return NULL;
    }
    out = lua_newuserdatauv(L, count * width, 0);
    memset(out, 0, count * width);
    lua_rawseti(L, a->table, ++a->next);
    return out;
}
static int strings(lua_State *L, int index, struct scratch *a,
                   const char *const **out, size_t *count, rotmos_error *error)
{
    const char **items;
    const char *text;
    size_t i, n, length;
    index = lua_absindex(L, index);
    if (!lua_istable(L, index))
        return 0;
    n = lua_rawlen(L, index);
    items = array(L, a, n, sizeof(*items), error);
    if (n && items == NULL)
        return 0;
    for (i = 0; i < n; ++i) {
        lua_rawgeti(L, index, (lua_Integer)i + 1);
        if (lua_type(L, -1) != LUA_TSTRING)
            return 0;
        text = lua_tolstring(L, -1, &length);
        if (memchr(text, 0, length) != NULL)
            return 0;
        items[i] = text;
        lua_pop(L, 1);
    }
    *out = items;
    *count = n;
    return 1;
}
static int material(lua_State *L, int owner, int index, const char *name,
                    rotmos_tls_material *out, rotmos_error *error)
{
    static const char *const allowed[] = {"kind", "path", "bytes", "open"};
    const char *kind = NULL;
    int value;
    size_t length;
    rl_field(L, index, name);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return 1;
    }
    if (lua_type(L, -1) == LUA_TSTRING) {
        const char *path = lua_tolstring(L, -1, &length);
        if (memchr(path, 0, length))
            return 0;
        out->kind = ROTMOS_MATERIAL_FILE;
        out->path = path;
        lua_pop(L, 1);
        return 1;
    }
    value = lua_absindex(L, -1);
    if (!lua_istable(L, value) ||
        !rl_fields(L, value, allowed, N(allowed), error) ||
        !rl_field_string(L, value, "kind", &kind) ||
        !rl_field_string(L, value, "path", &out->path))
        return 0;
    rl_field(L, value, "bytes");
    if (!lua_isnil(L, -1)) {
        if (lua_type(L, -1) != LUA_TSTRING)
            return 0;
        out->bytes.data = lua_tolstring(L, -1, &out->bytes.length);
    }
    lua_pop(L, 1);
    rl_field(L, value, "open");
    if ((kind != NULL && !strcmp(kind, "callback")) ||
        (kind == NULL && !lua_isnil(L, -1))) {
        lua_pop(L, 1);
        if (!rl_material_config(L, owner, value, out, error))
            return 0;
        lua_pop(L, 1);
        return 1;
    }
    if (!lua_isnil(L, -1))
        return 0;
    lua_pop(L, 1);
    if (kind == NULL)
        out->kind =
            out->path != NULL ? ROTMOS_MATERIAL_FILE : ROTMOS_MATERIAL_MEMORY;
    else if (!strcmp(kind, "file"))
        out->kind = ROTMOS_MATERIAL_FILE;
    else if (!strcmp(kind, "memory"))
        out->kind = ROTMOS_MATERIAL_MEMORY;
    else {
        rl_fail(error, ROTMOS_NOT_SUPPORTED,
                "TLS material kind must be file, memory or callback");
        return 0;
    }
    lua_pop(L, 1);
    return 1;
}
static int tls(lua_State *L, int owner, int index, rotmos_tls_config *c,
               struct scratch *a, rotmos_error *error)
{
    static const char *const allowed[] = {
        "version", "cipher_list", "max_material_bytes", "identities", "acme"};
    static const char *const identity_fields[] = {
        "mode",        "domains",
        "bundle",      "certificate",
        "private_key", "extra_chain",
        "client_ca",   "require_client_certificate"};
    const char *version = NULL, *mode;
    rotmos_tls_identity *identities;
    size_t i, n;
    int list, record;
    index = lua_absindex(L, index);
    if (!lua_istable(L, index) ||
        !rl_fields(L, index, allowed, N(allowed), error) ||
        !rl_field_string(L, index, "version", &version) ||
        !rl_field_string(L, index, "cipher_list", &c->cipher_list) ||
        !rl_field_size(L, index, "max_material_bytes", &c->max_material_bytes))
        return 0;
    if (version != NULL) {
        if (!strcmp(version, "default"))
            c->version = ROTMOS_TLS_VERSION_DEFAULT;
        else if (!strcmp(version, "both"))
            c->version = ROTMOS_TLS_VERSION_BOTH;
        else if (!strcmp(version, "1.2"))
            c->version = ROTMOS_TLS_VERSION_1_2;
        else if (!strcmp(version, "1.3"))
            c->version = ROTMOS_TLS_VERSION_1_3;
        else
            return 0;
    }
    rl_field(L, index, "acme");
    if (!lua_isnil(L, -1)) {
        static const struct setting acme_fields[] = {
            F(rotmos_acme_config, email, TEXT),
            F(rotmos_acme_config, directory_url, TEXT),
            F(rotmos_acme_config, runtime_directory, TEXT),
            F(rotmos_acme_config, ca_file, TEXT),
            F(rotmos_acme_config, ca_directory, TEXT),
            F(rotmos_acme_config, persist_timeout_ms, LONG)};
        static const char *const fields[] = {
            "email",   "directory_url", "runtime_directory",
            "ca_file", "ca_directory",  "persist_timeout_ms",
            "state"};
        int snapshot;
        if (!lua_istable(L, -1) || !rl_fields(L, -1, fields, N(fields), error))
            return 0;
        rl_table_copy(L, -1);
        snapshot = lua_absindex(L, -1);
        if (!rl_acme_config(L, owner, snapshot, &c->acme, error))
            return 0;
        lua_pushnil(L);
        lua_setfield(L, snapshot, "state");
        if (!settings(L, snapshot, &c->acme, acme_fields, N(acme_fields), 0,
                      error))
            return 0;
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    rl_field(L, index, "identities");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return 1;
    }
    if (!lua_istable(L, -1))
        return 0;
    list = lua_absindex(L, -1);
    n = lua_rawlen(L, list);
    identities = array(L, a, n, sizeof(*identities), error);
    if (n && identities == NULL)
        return 0;
    for (i = 0; i < n; ++i) {
        lua_rawgeti(L, list, (lua_Integer)i + 1);
        record = lua_absindex(L, -1);
        mode = NULL;
        if (!lua_istable(L, record) ||
            !rl_fields(L, record, identity_fields, N(identity_fields), error))
            return 0;
        rotmos_tls_identity_init(&identities[i]);
        if (!rl_field_string(L, record, "mode", &mode))
            return 0;
        identities[i].mode = ROTMOS_TLS_MANUAL;
        if (mode != NULL && strcmp(mode, "manual")) {
            if (!strcmp(mode, "disabled"))
                identities[i].mode = ROTMOS_TLS_DISABLED;
            else if (!strcmp(mode, "acme"))
                identities[i].mode = ROTMOS_TLS_ACME;
            else {
                rl_fail(error, ROTMOS_NOT_SUPPORTED,
                        "TLS identity mode must be disabled, manual or acme");
                return 0;
            }
        }
        rl_field(L, record, "domains");
        if (!strings(L, -1, a, &identities[i].domains,
                     &identities[i].domain_count, error))
            return 0;
        lua_pop(L, 1);
        if (!material(L, owner, record, "bundle", &identities[i].bundle,
                      error) ||
            !material(L, owner, record, "certificate",
                      &identities[i].certificate, error) ||
            !material(L, owner, record, "private_key",
                      &identities[i].private_key, error) ||
            !material(L, owner, record, "extra_chain",
                      &identities[i].extra_chain, error) ||
            !material(L, owner, record, "client_ca", &identities[i].client_ca,
                      error))
            return 0;
        rl_field(L, record, "require_client_certificate");
        if (!lua_isnil(L, -1)) {
            if (!lua_isboolean(L, -1))
                return 0;
            identities[i].require_client_certificate = lua_toboolean(L, -1);
        }
        lua_pop(L, 2);
    }
    c->identities = identities;
    c->identity_count = n;
    lua_pop(L, 1);
    return 1;
}
static int autoblock(lua_State *L, int index, rotmos_autoblock_config *c,
                     struct scratch *a, rotmos_error *error)
{
    static const char *const allowed[] = {
        "enabled",      "window_seconds",      "block_seconds",
        "max_entries",  "tcp_stall_threshold", "tls_failure_threshold",
        "status_rules", "event_rules"};
    int copy, list, record;
    size_t i, n;
    rotmos_autoblock_status_rule *statuses;
    rotmos_autoblock_event_rule *events;
    index = lua_absindex(L, index);
    if (!lua_istable(L, index) ||
        !rl_fields(L, index, allowed, N(allowed), error))
        return 0;
    rl_table_copy(L, index);
    copy = lua_absindex(L, -1);
    lua_pushnil(L);
    lua_setfield(L, copy, "status_rules");
    lua_pushnil(L);
    lua_setfield(L, copy, "event_rules");
    if (!settings(L, copy, c, blocking, N(blocking), 0, error))
        return 0;
    lua_pop(L, 1);
    rl_field(L, index, "status_rules");
    if (!lua_isnil(L, -1)) {
        if (!lua_istable(L, -1))
            return 0;
        list = lua_absindex(L, -1);
        n = lua_rawlen(L, list);
        statuses = array(L, a, n, sizeof(*statuses), error);
        if (n && statuses == NULL)
            return 0;
        for (i = 0; i < n; ++i) {
            lua_rawgeti(L, list, (lua_Integer)i + 1);
            record = lua_absindex(L, -1);
            if (!lua_istable(L, record) ||
                !settings(L, record, &statuses[i], status_rule, N(status_rule),
                          0, error))
                return 0;
            lua_pop(L, 1);
        }
        c->status_rules = statuses;
        c->status_rule_count = n;
    }
    lua_pop(L, 1);
    rl_field(L, index, "event_rules");
    if (!lua_isnil(L, -1)) {
        if (!lua_istable(L, -1))
            return 0;
        list = lua_absindex(L, -1);
        n = lua_rawlen(L, list);
        events = array(L, a, n, sizeof(*events), error);
        if (n && events == NULL)
            return 0;
        for (i = 0; i < n; ++i) {
            lua_rawgeti(L, list, (lua_Integer)i + 1);
            record = lua_absindex(L, -1);
            if (!lua_istable(L, record) ||
                !settings(L, record, &events[i], event_rule, N(event_rule), 0,
                          error))
                return 0;
            lua_pop(L, 1);
        }
        c->event_rules = events;
        c->event_rule_count = n;
    }
    lua_pop(L, 1);
    return 1;
}
int rl_config_parse(lua_State *L, int options, int owner,
                    rotmos_server_config *c, rotmos_error *error)
{
    static const char *const allowed[] = {
        "name",      "profile",     "listeners",    "signals",
        "workers",   "io",          "limits",       "http",
        "callbacks", "tls",         "timeouts",     "proxy",
        "client_ip", "autoblock",   "start_paused", "correlation_id",
        "admission", "supervision", "logger",       "access_log"};
    static const char *const callbacks[] = {
        "on_process_start",   "on_parent_prepare", "on_parent_ready",
        "on_parent_stopping", "on_parent_stopped", "on_worker_start",
        "on_worker_stopping", "on_worker_exit",    "on_worker_event",
        "on_tls_identity",    "on_request_start",  "on_request_close",
        "on_observe"};
    static const char *const listener_fields[] = {"bind",
                                                  "name",
                                                  "port",
                                                  "kind",
                                                  "identity_index",
                                                  "presented_port",
                                                  "redirect_https_port"};
    static const char *const client_fields[] = {"trusted_proxies", "resolve"};
    static const char *const signal_fields[] = {"mode"};
    struct scratch a;
    rotmos_listener_config *listeners;
    const char *kind = NULL;
    size_t n, i, port, identity, presented, redirect;
    int list, record;
    options = lua_absindex(L, options);
    rotmos_error_clear(error);
    lua_newtable(L);
    a.table = lua_absindex(L, -1);
    a.next = 0;
    if (!rl_fields(L, options, allowed, N(allowed), error) ||
        !rl_field_string(L, options, "profile", &kind))
        goto invalid;
    if (kind != NULL) {
        if (strcmp(kind, "production_webserver"))
            goto invalid;
        rotmos_server_config_init_production_webserver(c);
    }
    c->timeouts.deferred_response.disabled = 1;
    if (!rl_field_string(L, options, "name", &c->name))
        goto invalid;
    rl_field(L, options, "callbacks");
    if (!lua_isnil(L, -1)) {
        if (!lua_istable(L, -1) ||
            !rl_fields(L, -1, callbacks, N(callbacks), error))
            goto invalid;
        for (i = 0; i < N(callbacks); ++i) {
            rl_field(L, -1, callbacks[i]);
            if (!lua_isnil(L, -1) && !lua_isfunction(L, -1))
                goto invalid;
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
#define GROUP(key, member, fields)                                             \
    do {                                                                       \
        rl_field(L, options, key);                                             \
        if (!lua_isnil(L, -1) &&                                               \
            (!lua_istable(L, -1) ||                                            \
             !settings(L, -1, &c->member, fields, N(fields), a.table, error))) \
            goto invalid;                                                      \
        lua_pop(L, 1);                                                         \
    } while (0)
    GROUP("workers", workers, workers);
    GROUP("io", io, io);
    GROUP("limits", limits, limits);
    GROUP("http", http, http);
    GROUP("timeouts", timeouts, timeouts);
    GROUP("proxy", proxy, proxy);
#undef GROUP
    rl_field(L, options, "start_paused");
    if (!lua_isnil(L, -1)) {
        if (!lua_isboolean(L, -1))
            goto invalid;
        c->start_paused = lua_toboolean(L, -1);
    }
    lua_pop(L, 1);
    rl_field(L, options, "signals");
    kind = NULL;
    if (!lua_isnil(L, -1)) {
        if (!lua_istable(L, -1) ||
            !rl_fields(L, -1, signal_fields, N(signal_fields), error) ||
            !rl_field_string(L, -1, "mode", &kind))
            goto invalid;
        if (kind != NULL) {
            if (!strcmp(kind, "library"))
                c->signals.mode = ROTMOS_SIGNALS_LIBRARY;
            else if (!strcmp(kind, "host"))
                c->signals.mode = ROTMOS_SIGNALS_HOST;
            else
                goto invalid;
        }
    }
    lua_pop(L, 1);
    rl_field(L, options, "client_ip");
    if (!lua_isnil(L, -1)) {
        if (!lua_istable(L, -1) ||
            !rl_fields(L, -1, client_fields, N(client_fields), error))
            goto invalid;
        rl_field(L, -1, "trusted_proxies");
        if (!lua_isnil(L, -1) &&
            !strings(L, -1, &a, &c->client_ip.trusted_proxies,
                     &c->client_ip.trusted_proxy_count, error))
            goto invalid;
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    rl_field(L, options, "autoblock");
    if (!lua_isnil(L, -1) && !autoblock(L, -1, &c->autoblock, &a, error))
        goto invalid;
    lua_pop(L, 1);
    rl_field(L, options, "tls");
    if (!lua_isnil(L, -1) && !tls(L, owner, -1, &c->tls, &a, error))
        goto invalid;
    lua_pop(L, 1);
    rl_field(L, options, "listeners");
    if (!lua_istable(L, -1))
        goto invalid;
    list = lua_absindex(L, -1);
    n = lua_rawlen(L, list);
    if (n == 0 || n > 255)
        goto invalid;
    listeners = array(L, &a, n, sizeof(*listeners), error);
    if (listeners == NULL)
        goto invalid;
    for (i = 0; i < n; ++i) {
        lua_rawgeti(L, list, (lua_Integer)i + 1);
        record = lua_absindex(L, -1);
        if (!lua_istable(L, record) ||
            !rl_fields(L, record, listener_fields, N(listener_fields), error))
            goto invalid;
        rotmos_listener_config_init(&listeners[i]);
        port = presented = redirect = 0;
        identity = 1;
        kind = NULL;
        if (!rl_field_string(L, record, "bind", &listeners[i].bind) ||
            !rl_field_string(L, record, "name", &listeners[i].name) ||
            !rl_field_size(L, record, "port", &port) || port > 65535 ||
            !rl_field_size(L, record, "identity_index", &identity) ||
            identity == 0 ||
            !rl_field_size(L, record, "presented_port", &presented) ||
            presented > 65535 ||
            !rl_field_size(L, record, "redirect_https_port", &redirect) ||
            redirect > 65535 || !rl_field_string(L, record, "kind", &kind))
            goto invalid;
        listeners[i].port = (unsigned short)port;
        listeners[i].presented_port = (unsigned short)presented;
        listeners[i].redirect_https_port = (unsigned short)redirect;
        listeners[i].identity_index = identity - 1;
        if (kind != NULL) {
            if (!strcmp(kind, "https"))
                listeners[i].kind = ROTMOS_LISTENER_HTTPS;
            else if (!strcmp(kind, "redirect"))
                listeners[i].kind = ROTMOS_LISTENER_REDIRECT;
            else if (strcmp(kind, "http"))
                goto invalid;
        }
        lua_pop(L, 1);
    }
    c->listeners = listeners;
    c->listener_count = n;
    lua_settop(L, a.table);
    return 1;
invalid:
    if (error->status == ROTMOS_OK)
        rl_fail(error, ROTMOS_INVALID,
                "invalid server configuration field or integer range");
    return 0;
}
static void settings_push(lua_State *L, const void *config,
                          const struct setting *fields, size_t count)
{
    const unsigned char *base = config;
    size_t i;
    lua_newtable(L);
    for (i = 0; i < count; ++i) {
        const void *value = base + fields[i].offset;
        switch (fields[i].kind) {
        case SIZE_FIELD:
            rl_push_size(L, *(const size_t *)value);
            break;
        case UINT_FIELD:
            lua_pushinteger(L, *(const unsigned int *)value);
            break;
        case LONG_FIELD:
            lua_pushinteger(L, *(const long *)value);
            break;
        case ULONG_FIELD:
            lua_pushinteger(L, (lua_Integer) * (const unsigned long *)value);
            break;
        case BOOL_FIELD:
            lua_pushboolean(L, *(const int *)value);
            break;
        case WIDE_FIELD:
            rl_push_u64(L, *(const rotmos_u64 *)value);
            break;
        case TEXT_FIELD:
            lua_pushstring(L, *(const char *const *)value);
            break;
        case MIME_FIELD:
            rl_mime_push(L, *(const rotmos_mime_type *const *)value,
                         ((const rotmos_http_config *)config)->mime_type_count);
            break;
        case DEADLINE_FIELD:
            settings_push(L, value, deadline, N(deadline));
            break;
        case DEATH_FIELD:
            lua_pushstring(L, *(const rotmos_worker_death_policy *)value ==
                                      ROTMOS_WORKER_RESTART
                                  ? "restart"
                                  : "terminate");
            break;
        }
        lua_setfield(L, -2, fields[i].name);
    }
}
void rl_config_push(lua_State *L, const rotmos_server_effective *e)
{
    const rotmos_server_config *c = &e->config;
    size_t i, j;
    static const char *materials[] = {"bundle", "certificate", "private_key",
                                      "extra_chain", "client_ca"};
    static const char *versions[] = {"default", "both", "1.2", "1.3"};
    static const char *kinds[] = {"none", "file", "memory", "callback"};
    lua_newtable(L);
#define PUSH(n)                                                                \
    settings_push(L, &c->n, n, N(n));                                          \
    lua_setfield(L, -2, #n)
    PUSH(workers);
    PUSH(io);
    PUSH(limits);
    PUSH(http);
    PUSH(timeouts);
    PUSH(proxy);
#undef PUSH
    settings_push(L, &c->autoblock, blocking, N(blocking));
    lua_setfield(L, -2, "autoblock");
    settings_push(L, &e->requested_timeouts, timeouts, N(timeouts));
    lua_setfield(L, -2, "requested_timeouts");
    rl_push_size(L, e->requested_worker_count);
    lua_setfield(L, -2, "requested_worker_count");
    rl_push_size(L, e->max_header_fields);
    lua_setfield(L, -2, "max_header_fields");
    lua_pushstring(L, e->backend_name);
    lua_setfield(L, -2, "backend_name");
    lua_pushstring(L, c->name);
    lua_setfield(L, -2, "name");
    lua_pushboolean(L, c->start_paused);
    lua_setfield(L, -2, "start_paused");
    lua_createtable(L, (int)c->listener_count, 0);
    for (i = 0; i < c->listener_count; ++i) {
        const rotmos_listener_config *p = &c->listeners[i];
        lua_newtable(L);
        lua_pushstring(L, p->name);
        lua_setfield(L, -2, "name");
        lua_pushstring(L, p->bind);
        lua_setfield(L, -2, "bind");
        lua_pushinteger(L, p->port);
        lua_setfield(L, -2, "port");
        lua_pushstring(L, p->kind == ROTMOS_LISTENER_HTTP    ? "http"
                          : p->kind == ROTMOS_LISTENER_HTTPS ? "https"
                                                             : "redirect");
        lua_setfield(L, -2, "kind");
        rl_push_size(L, p->identity_index + 1);
        lua_setfield(L, -2, "identity_index");
        lua_pushinteger(L, p->presented_port);
        lua_setfield(L, -2, "presented_port");
        lua_pushinteger(L, p->redirect_https_port);
        lua_setfield(L, -2, "redirect_https_port");
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    lua_setfield(L, -2, "listeners");
    lua_newtable(L);
    lua_pushstring(L,
                   c->signals.mode == ROTMOS_SIGNALS_HOST ? "host" : "library");
    lua_setfield(L, -2, "mode");
    lua_setfield(L, -2, "signals");
    lua_newtable(L);
    lua_pushboolean(L, c->supervision.enabled);
    lua_setfield(L, -2, "enabled");
    lua_pushinteger(L, c->supervision.control_fd);
    lua_setfield(L, -2, "control_fd");
    lua_pushinteger(L, c->supervision.interest);
    lua_setfield(L, -2, "interest");
    lua_setfield(L, -2, "supervision");
    lua_newtable(L);
    lua_newtable(L);
    for (i = 0; i < c->client_ip.trusted_proxy_count; ++i) {
        lua_pushstring(L, c->client_ip.trusted_proxies[i]);
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    lua_setfield(L, -2, "trusted_proxies");
    lua_setfield(L, -2, "client_ip");
    lua_getfield(L, -1, "autoblock");
    lua_newtable(L);
    for (i = 0; i < c->autoblock.status_rule_count; ++i) {
        settings_push(L, &c->autoblock.status_rules[i], status_rule,
                      N(status_rule));
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    lua_setfield(L, -2, "status_rules");
    lua_newtable(L);
    for (i = 0; i < c->autoblock.event_rule_count; ++i) {
        settings_push(L, &c->autoblock.event_rules[i], event_rule,
                      N(event_rule));
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    lua_setfield(L, -2, "event_rules");
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushstring(L, versions[c->tls.version]);
    lua_setfield(L, -2, "version");
    lua_pushstring(L, c->tls.cipher_list);
    lua_setfield(L, -2, "cipher_list");
    rl_push_size(L, c->tls.max_material_bytes);
    lua_setfield(L, -2, "max_material_bytes");
    lua_newtable(L);
    lua_pushstring(L, c->tls.acme.email);
    lua_setfield(L, -2, "email");
    lua_pushstring(L, c->tls.acme.directory_url);
    lua_setfield(L, -2, "directory_url");
    lua_pushstring(L, c->tls.acme.runtime_directory);
    lua_setfield(L, -2, "runtime_directory");
    lua_pushstring(L, c->tls.acme.ca_file);
    lua_setfield(L, -2, "ca_file");
    lua_pushstring(L, c->tls.acme.ca_directory);
    lua_setfield(L, -2, "ca_directory");
    lua_pushinteger(L, c->tls.acme.persist_timeout_ms);
    lua_setfield(L, -2, "persist_timeout_ms");
    lua_setfield(L, -2, "acme");
    lua_newtable(L);
    for (i = 0; i < c->tls.identity_count; ++i) {
        const rotmos_tls_identity *id = &c->tls.identities[i];
        const rotmos_tls_material *m[5];
        m[0] = &id->bundle;
        m[1] = &id->certificate;
        m[2] = &id->private_key;
        m[3] = &id->extra_chain;
        m[4] = &id->client_ca;
        lua_newtable(L);
        lua_pushstring(L, id->mode == ROTMOS_TLS_DISABLED ? "disabled"
                          : id->mode == ROTMOS_TLS_MANUAL ? "manual"
                                                          : "acme");
        lua_setfield(L, -2, "mode");
        lua_pushboolean(L, id->require_client_certificate);
        lua_setfield(L, -2, "require_client_certificate");
        lua_newtable(L);
        for (j = 0; j < id->domain_count; ++j) {
            lua_pushstring(L, id->domains[j]);
            lua_rawseti(L, -2, (lua_Integer)j + 1);
        }
        lua_setfield(L, -2, "domains");
        for (j = 0; j < 5; ++j) {
            lua_newtable(L);
            lua_pushstring(L, kinds[m[j]->kind]);
            lua_setfield(L, -2, "kind");
            lua_pushstring(L, m[j]->path);
            lua_setfield(L, -2, "path");
            if (m[j]->bytes.data != NULL) {
                lua_pushlstring(L, m[j]->bytes.data, m[j]->bytes.length);
                lua_setfield(L, -2, "bytes");
            }
            lua_setfield(L, -2, materials[j]);
        }
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    lua_setfield(L, -2, "identities");
    lua_setfield(L, -2, "tls");
}
