/* Option families adapted from Vectis's MIT-licensed Lua curl facade. */
#define _POSIX_C_SOURCE 200809L
#include "curl_internal.h"
#include <fcntl.h>
#include <unistd.h>

struct option {
    const char *name;
    CURLoption option;
    long fallback;
};
static int token(const char *s, size_t length)
{
    size_t i;
    if (!length)
        return 0;
    for (i = 0; i < length; ++i)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
              (s[i] >= '0' && s[i] <= '9') ||
              (s[i] != '\0' && strchr("!#$%&'*+-.^_`|~", s[i]))))
            return 0;
    return 1;
}
static int string(lua_State *L, int options, const char *name, const char **out)
{
    return rl_field_string(L, options, name, out);
}
static knurl_status invalid(knurl_error *error, const char *message)
{
    return rl_fail(error, KNURL_INVALID, message);
}
static int list_add(struct curl_slist **list, const char *text)
{
    struct curl_slist *next = curl_slist_append(*list, text);
    if (next == NULL)
        return 0;
    *list = next;
    return 1;
}
static knurl_status headers(lua_State *L, struct rl_curl_call *c,
                            knurl_error *error)
{
    const char *key, *value, *colon;
    size_t key_length, length, total = 0, count = 0;
    char *line;
    int options;
    rl_field(L, 1, "headers");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return KNURL_OK;
    }
    if (!lua_istable(L, -1))
        return invalid(error, "curl headers require a table");
    options = lua_absindex(L, -1);
    lua_pushnil(L);
    while (lua_next(L, options)) {
        if (++count > 128 || lua_type(L, -1) != LUA_TSTRING)
            return invalid(error, "invalid curl header");
        value = lua_tolstring(L, -1, &length);
        if (memchr(value, 0, length) || memchr(value, '\r', length) ||
            memchr(value, '\n', length))
            return invalid(error, "curl headers cannot contain NUL, CR or LF");
        if (lua_type(L, -2) == LUA_TSTRING) {
            key = lua_tolstring(L, -2, &key_length);
            if (!token(key, key_length) || key_length > 256 ||
                length > RL_CURL_HEADER_LIMIT - key_length - 2)
                return invalid(error, "invalid curl header name or length");
            line = malloc(key_length + length + 3);
            if (line == NULL)
                return rl_fail(error, KNURL_NOMEM, "allocating curl header");
            memcpy(line, key, key_length);
            memcpy(line + key_length, ": ", 2);
            memcpy(line + key_length + 2, value, length + 1);
            length += key_length + 2;
            if (!list_add(&c->headers, line)) {
                free(line);
                return rl_fail(error, KNURL_NOMEM, "copying curl header");
            }
            free(line);
        } else if (lua_isinteger(L, -2)) {
            colon = memchr(value, ':', length);
            if (colon == NULL || !token(value, (size_t)(colon - value)))
                return invalid(error, "invalid raw curl header");
            if (!list_add(&c->headers, value))
                return rl_fail(error, KNURL_NOMEM, "copying raw curl header");
        } else
            return invalid(error, "invalid curl header key");
        if (length > RL_CURL_HEADER_LIMIT - total)
            return invalid(error, "curl request headers exceed 64 KiB");
        total += length;
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    if (curl_easy_setopt(c->easy, CURLOPT_HTTPHEADER, c->headers) != CURLE_OK)
        return rl_fail(error, KNURL_BACKEND, "setting curl headers");
    return KNURL_OK;
}
static size_t mime_read(char *buffer, size_t size, size_t count, void *data)
{
    FILE *stream = data;
    size_t read = fread(buffer, 1, size * count, stream);
    return ferror(stream) ? CURL_READFUNC_ABORT : read;
}

static int mime_seek(void *data, curl_off_t offset, int origin)
{
    return (curl_off_t)(off_t)offset == offset &&
                   fseeko(data, (off_t)offset, origin) == 0
               ? CURL_SEEKFUNC_OK
               : CURL_SEEKFUNC_FAIL;
}

static knurl_status mime_file(struct rl_curl_call *c, curl_mimepart *part,
                              const char *path, const char *filename,
                              knurl_error *error)
{
    struct rl_curl_mime_file *file = malloc(sizeof(*file));
    int fd;
    if (file == NULL)
        return rl_fail(error, KNURL_NOMEM, "tracking curl MIME file");
    fd = rl_curl_open_file(path, O_RDONLY, &file->info);
    if (fd < 0 || (file->stream = fdopen(fd, "rb")) == NULL) {
        if (fd >= 0)
            close(fd);
        free(file);
        return rl_fail(error, KNURL_IO,
                       "curl MIME path must be a readable regular file");
    }
    file->next = c->mime_files;
    c->mime_files = file;
    if (filename == NULL) {
        filename = strrchr(path, '/');
        filename = filename != NULL ? filename + 1 : path;
    }
    if (curl_mime_data_cb(part, (curl_off_t)file->info.st_size, mime_read,
                          mime_seek, NULL, file->stream) != CURLE_OK ||
        curl_mime_filename(part, filename) != CURLE_OK)
        return rl_fail(error, KNURL_BACKEND, "configuring curl MIME file");
    return KNURL_OK;
}

static knurl_status multipart(lua_State *L, struct rl_curl_call *c,
                              knurl_error *error)
{
    curl_mimepart *part;
    const char *name, *value, *path, *filename, *type;
    size_t length, count = 0;
    int table, item;
    knurl_status status;
    rl_field(L, 1, "multipart");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return KNURL_OK;
    }
    if (!lua_istable(L, -1))
        return invalid(error, "curl multipart requires a table");
    c->mime = curl_mime_init(c->easy);
    if (c->mime == NULL)
        return rl_fail(error, KNURL_NOMEM, "allocating curl multipart");
    table = lua_absindex(L, -1);
    lua_pushnil(L);
    while (lua_next(L, table)) {
        if (++count > 128)
            return invalid(error, "curl multipart limit is 128 parts");
        name = value = path = filename = type = NULL;
        if (lua_type(L, -2) == LUA_TSTRING) {
            name = lua_tolstring(L, -2, &length);
            if (memchr(name, 0, length))
                return invalid(error, "curl MIME name contains NUL");
        }
        if (lua_istable(L, -1)) {
            item = lua_absindex(L, -1);
            if (!string(L, item, "name", &name) ||
                !string(L, item, "path", &path) ||
                !string(L, item, "file_path", &path) ||
                !string(L, item, "filename", &filename) ||
                !string(L, item, "content_type", &type))
                return invalid(error, "invalid curl MIME fields");
            rl_field(L, item, "value");
            if (lua_isnil(L, -1)) {
                lua_pop(L, 1);
                rl_field(L, item, "body");
            }
        } else
            lua_pushvalue(L, -1);
        if (!lua_isnil(L, -1)) {
            if (lua_type(L, -1) != LUA_TSTRING)
                return invalid(error, "curl MIME values must be strings");
            value = lua_tolstring(L, -1, &length);
        } else
            length = 0;
        if (name == NULL || (path != NULL && value != NULL) ||
            (path == NULL && value == NULL))
            return invalid(
                error,
                "curl MIME part requires a name and exactly one value/file");
        part = curl_mime_addpart(c->mime);
        if (part == NULL || curl_mime_name(part, name) != CURLE_OK ||
            (type && curl_mime_type(part, type) != CURLE_OK))
            return rl_fail(error, KNURL_BACKEND, "configuring curl MIME part");
        if (path != NULL) {
            status = mime_file(c, part, path, filename, error);
            if (status != KNURL_OK)
                return status;
        } else if (curl_mime_data(part, value, length) != CURLE_OK ||
                   (filename && curl_mime_filename(part, filename) != CURLE_OK))
            return rl_fail(error, KNURL_BACKEND, "configuring curl MIME part");
        lua_pop(L, 2);
    }
    lua_pop(L, 1);
    if (curl_easy_setopt(c->easy, CURLOPT_MIMEPOST, c->mime) != CURLE_OK)
        return rl_fail(error, KNURL_BACKEND, "configuring curl MIME body");
    return KNURL_OK;
}
static knurl_status smtp(lua_State *L, struct rl_curl_call *c,
                         knurl_error *error)
{
    const char *from = NULL, *recipient;
    size_t i, count;
    int table, use_ssl = 0;
    rl_field(L, 1, "smtp");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return KNURL_OK;
    }
    if (!lua_istable(L, -1))
        return invalid(error, "curl smtp requires a table");
    table = lua_absindex(L, -1);
    if (!string(L, table, "mail_from", &from) || from == NULL ||
        strpbrk(from, "\r\n") || !rl_field_bool(L, table, "use_ssl", &use_ssl))
        return invalid(error, "invalid curl SMTP configuration");
    rl_field(L, table, "rcpt");
    if (!lua_istable(L, -1) || (count = lua_rawlen(L, -1)) == 0 || count > 128)
        return invalid(error, "curl SMTP requires 1..128 recipients");
    for (i = 0; i < count; ++i) {
        lua_rawgeti(L, -1, (lua_Integer)i + 1);
        if (lua_type(L, -1) != LUA_TSTRING)
            return invalid(error, "invalid curl SMTP recipient");
        recipient = lua_tostring(L, -1);
        if (strlen(recipient) != lua_rawlen(L, -1) ||
            strpbrk(recipient, "\r\n") || !list_add(&c->recipients, recipient))
            return invalid(error,
                           "invalid curl SMTP recipient or allocation failure");
        lua_pop(L, 1);
    }
    lua_pop(L, 2);
    if (curl_easy_setopt(c->easy, CURLOPT_MAIL_FROM, from) != CURLE_OK ||
        curl_easy_setopt(c->easy, CURLOPT_MAIL_RCPT, c->recipients) !=
            CURLE_OK ||
        curl_easy_setopt(c->easy, CURLOPT_USE_SSL,
                         use_ssl ? CURLUSESSL_ALL : CURLUSESSL_NONE) !=
            CURLE_OK ||
        curl_easy_setopt(c->easy, CURLOPT_UPLOAD, 1L) != CURLE_OK)
        return rl_fail(error, KNURL_BACKEND, "configuring curl SMTP");
    return KNURL_OK;
}
knurl_status rl_curl_config(lua_State *L, struct rl_curl_call *c,
                            knurl_error *error)
{
    static const char *const allowed[] = {"url",
                                          "username",
                                          "password",
                                          "proxy",
                                          "proxy_username",
                                          "proxy_password",
                                          "interface",
                                          "user_agent",
                                          "accept_encoding",
                                          "ca_file",
                                          "ca_path",
                                          "client_cert",
                                          "client_key",
                                          "client_cert_type",
                                          "key_password",
                                          "ssh_private_key",
                                          "ssh_public_key",
                                          "ssh_known_hosts",
                                          "timeout_ms",
                                          "connect_timeout_ms",
                                          "low_speed_limit",
                                          "low_speed_time",
                                          "proxy_type",
                                          "verify_peer",
                                          "verify_host",
                                          "follow_redirects",
                                          "tcp_keepalive",
                                          "no_signal",
                                          "http2",
                                          "upload",
                                          "protocols",
                                          "method",
                                          "headers",
                                          "read",
                                          "write",
                                          "progress",
                                          "upload_path",
                                          "body_path",
                                          "download_path",
                                          "body",
                                          "multipart",
                                          "smtp",
                                          "request_schema",
                                          "response_schema",
                                          "body_json"};
    static const struct option strings[] = {
        {"url", CURLOPT_URL, 0},
        {"username", CURLOPT_USERNAME, 0},
        {"password", CURLOPT_PASSWORD, 0},
        {"proxy", CURLOPT_PROXY, 0},
        {"proxy_username", CURLOPT_PROXYUSERNAME, 0},
        {"proxy_password", CURLOPT_PROXYPASSWORD, 0},
        {"interface", CURLOPT_INTERFACE, 0},
        {"user_agent", CURLOPT_USERAGENT, 0},
        {"accept_encoding", CURLOPT_ACCEPT_ENCODING, 0},
        {"ca_file", CURLOPT_CAINFO, 0},
        {"ca_path", CURLOPT_CAPATH, 0},
        {"client_cert", CURLOPT_SSLCERT, 0},
        {"client_key", CURLOPT_SSLKEY, 0},
        {"client_cert_type", CURLOPT_SSLCERTTYPE, 0},
        {"key_password", CURLOPT_KEYPASSWD, 0},
        {"ssh_private_key", CURLOPT_SSH_PRIVATE_KEYFILE, 0},
        {"ssh_public_key", CURLOPT_SSH_PUBLIC_KEYFILE, 0},
        {"ssh_known_hosts", CURLOPT_SSH_KNOWNHOSTS, 0}};
    static const struct option numbers[] = {
        {"timeout_ms", CURLOPT_TIMEOUT_MS, 30000},
        {"connect_timeout_ms", CURLOPT_CONNECTTIMEOUT_MS, 10000},
        {"low_speed_limit", CURLOPT_LOW_SPEED_LIMIT, 0},
        {"low_speed_time", CURLOPT_LOW_SPEED_TIME, 0},
        {"proxy_type", CURLOPT_PROXYTYPE, CURLPROXY_HTTP}};
    static const struct option booleans[] = {
        {"verify_peer", CURLOPT_SSL_VERIFYPEER, 1},
        {"follow_redirects", CURLOPT_FOLLOWLOCATION, 0},
        {"tcp_keepalive", CURLOPT_TCP_KEEPALIVE, 1}};
    const char *value, *method = NULL, *path = NULL, *protocols = "http,https",
                       *ca, *ca_file = NULL, *ca_path = NULL;
    size_t i, number, length;
    int enabled, upload = 0, sources, has_body = 0;
    knurl_status status;
    if (!rl_fields(L, 1, allowed, sizeof(allowed) / sizeof(allowed[0]), error))
        return KNURL_INVALID;
    if (!string(L, 1, "url", &value) || value == NULL || *value == '\0')
        return invalid(error, "curl requires a URL without NUL bytes");
    enabled = 1;
    if (!rl_field_bool(L, 1, "no_signal", &enabled) || !enabled)
        return invalid(error, "curl requires no_signal=true");
    if (curl_easy_setopt(c->easy, CURLOPT_NOSIGNAL, 1L) != CURLE_OK)
        return rl_fail(error, KNURL_BACKEND,
                       "configuring curl signal handling");
    if (!string(L, 1, "ca_file", &ca_file) ||
        !string(L, 1, "ca_path", &ca_path) ||
        curl_easy_setopt(c->easy, CURLOPT_CAINFO, NULL) != CURLE_OK ||
        curl_easy_setopt(c->easy, CURLOPT_CAPATH, NULL) != CURLE_OK)
        return invalid(error, "invalid curl trust configuration");
    ca = ca_file == NULL && ca_path == NULL ? knurl_tls_system_ca_file() : NULL;
    if (ca != NULL && curl_easy_setopt(c->easy, CURLOPT_CAINFO, ca) != CURLE_OK)
        return rl_fail(error, KNURL_BACKEND, "configuring system CA bundle");
    ca = ca_file == NULL && ca_path == NULL ? getenv("SSL_CERT_DIR") : NULL;
    if (ca != NULL && *ca &&
        curl_easy_setopt(c->easy, CURLOPT_CAPATH, ca) != CURLE_OK)
        return rl_fail(error, KNURL_BACKEND, "configuring system CA directory");
    for (i = 0; i < sizeof(strings) / sizeof(strings[0]); ++i) {
        value = NULL;
        if (!string(L, 1, strings[i].name, &value))
            return invalid(error,
                           "curl string option contains an invalid value");
        if (value != NULL &&
            curl_easy_setopt(c->easy, strings[i].option, value) != CURLE_OK)
            return invalid(error, "curl string option was rejected");
    }
    for (i = 0; i < sizeof(numbers) / sizeof(numbers[0]); ++i) {
        number = (size_t)numbers[i].fallback;
        if (!rl_field_size(L, 1, numbers[i].name, &number) || number > LONG_MAX)
            return invalid(error, "curl numeric option must be a nonnegative "
                                  "integer within LONG_MAX");
        if (curl_easy_setopt(c->easy, numbers[i].option, (long)number) !=
            CURLE_OK)
            return invalid(error, "curl numeric option was rejected");
    }
    for (i = 0; i < sizeof(booleans) / sizeof(booleans[0]); ++i) {
        enabled = (int)booleans[i].fallback;
        if (!rl_field_bool(L, 1, booleans[i].name, &enabled))
            return invalid(error, "curl boolean option must be a boolean");
        if (curl_easy_setopt(c->easy, booleans[i].option, (long)enabled) !=
            CURLE_OK)
            return invalid(error, "curl boolean option was rejected");
    }
    enabled = 1;
    if (!rl_field_bool(L, 1, "verify_host", &enabled) ||
        curl_easy_setopt(c->easy, CURLOPT_SSL_VERIFYHOST, enabled ? 2L : 0L) !=
            CURLE_OK)
        return invalid(error, "invalid curl host verification option");
    enabled = 0;
    if (!rl_field_bool(L, 1, "http2", &enabled) ||
        !rl_field_bool(L, 1, "upload", &upload) ||
        !string(L, 1, "protocols", &protocols) ||
        !string(L, 1, "method", &method) ||
        (method != NULL && !token(method, strlen(method))))
        return invalid(error,
                       "invalid curl method, protocols or upload option");
    rl_field(L, 1, "smtp");
    if (!lua_isnil(L, -1))
        upload = 1;
    lua_pop(L, 1);
    if (curl_easy_setopt(c->easy, CURLOPT_HTTP_VERSION,
                         enabled ? CURL_HTTP_VERSION_2TLS
                                 : CURL_HTTP_VERSION_NONE) != CURLE_OK ||
        curl_easy_setopt(c->easy, CURLOPT_PROTOCOLS_STR, protocols) !=
            CURLE_OK ||
        curl_easy_setopt(c->easy, CURLOPT_REDIR_PROTOCOLS_STR, protocols) !=
            CURLE_OK)
        return invalid(error, "unsupported curl protocol selection");
    status = headers(L, c, error);
    if (status != KNURL_OK)
        return status;
    for (i = 0; i < 3; ++i) {
        static const char *const callbacks[] = {"read", "write", "progress"};
        rl_field(L, 1, callbacks[i]);
        if (!lua_isnil(L, -1) && !lua_isfunction(L, -1))
            return invalid(error, "curl callback must be a function");
        enabled = lua_isfunction(L, -1);
        lua_pop(L, 1);
        if (i == 0)
            c->read_callback = enabled;
        else if (i == 1)
            c->write_callback = enabled;
        else
            c->progress_callback = enabled;
    }
    if (!string(L, 1, "upload_path", &path) ||
        !string(L, 1, "body_path", &path))
        return invalid(error, "invalid curl upload path");
    if (path != NULL) {
        c->input_path = malloc(strlen(path) + 1);
        if (c->input_path == NULL)
            return rl_fail(error, KNURL_NOMEM, "copying curl upload path");
        strcpy(c->input_path, path);
    }
    path = NULL;
    if (!string(L, 1, "download_path", &path))
        return invalid(error, "invalid curl download path");
    if (path != NULL) {
        c->output_path = malloc(strlen(path) + 1);
        if (c->output_path == NULL)
            return rl_fail(error, KNURL_NOMEM, "copying curl download path");
        strcpy(c->output_path, path);
    }
    c->download = path != NULL;
    status = rl_curl_json_config(L, c, error);
    if (status != KNURL_OK)
        return status;
    sources = c->read_callback + (c->input_path != NULL) + c->json_read;
    rl_field(L, 1, "body");
    if (!lua_isnil(L, -1)) {
        has_body = 1;
        if (lua_type(L, -1) != LUA_TSTRING || sources != 0)
            return invalid(error,
                           "curl upload requires exactly one body source");
        value = lua_tolstring(L, -1, &length);
        if (length > RL_CURL_BODY_LIMIT)
            return invalid(error, "curl buffered upload exceeds 8 MiB");
        if (upload) {
            c->request_body = malloc(length ? length : 1);
            if (c->request_body == NULL)
                return rl_fail(error, KNURL_NOMEM, "copying curl upload body");
            memcpy(c->request_body, value, length);
            c->request_length = length;
            sources = 1;
        } else if (curl_easy_setopt(c->easy, CURLOPT_POSTFIELDSIZE_LARGE,
                                    (curl_off_t)length) != CURLE_OK ||
                   curl_easy_setopt(c->easy, CURLOPT_COPYPOSTFIELDS, value) !=
                       CURLE_OK)
            return rl_fail(error, KNURL_BACKEND, "configuring curl body");
    }
    lua_pop(L, 1);
    if (sources > 1 ||
        (c->output_path != NULL && (c->write_callback || c->json_write)) ||
        (c->write_callback && c->json_write))
        return invalid(error, "curl body sources/sinks are mutually exclusive");
    status = multipart(L, c, error);
    if (status != KNURL_OK)
        return status;
    if (c->mime != NULL && (sources != 0 || upload || has_body))
        return invalid(error,
                       "curl MIME and upload bodies are mutually exclusive");
    if ((upload || sources) &&
        curl_easy_setopt(c->easy, CURLOPT_UPLOAD, 1L) != CURLE_OK)
        return rl_fail(error, KNURL_BACKEND,
                       "configuring curl streaming upload");
    if (method != NULL) {
        if (strcmp(method, "HEAD") == 0 &&
            curl_easy_setopt(c->easy, CURLOPT_NOBODY, 1L) != CURLE_OK)
            return invalid(error, "cannot configure curl HEAD");
        if (curl_easy_setopt(c->easy, CURLOPT_CUSTOMREQUEST, method) !=
            CURLE_OK)
            return invalid(error, "cannot configure curl method");
    }
    return smtp(L, c, error);
}
