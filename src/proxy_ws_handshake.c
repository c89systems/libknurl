#include "proxy_ws_handshake.h"

#include <openssl/evp.h>
#include <string.h>
#include <strings.h>

#define KNURL_PROXY_WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

typedef struct knurl_proxy_ws_name {
    const char *text;
    size_t length;
} knurl_proxy_ws_name;

static int knurl_proxy_ws_fail(const char **reason, const char *message)
{
    if (reason != NULL)
        *reason = message;
    return 0;
}

static int knurl_proxy_ws_token(unsigned char value)
{
    if ((value >= '0' && value <= '9') || (value >= 'A' && value <= 'Z') ||
        (value >= 'a' && value <= 'z'))
        return 1;
    return strchr("!#$%&'*+-.^_`|~", (int)value) != NULL;
}

static void knurl_proxy_ws_trim(const char **start, size_t *length)
{
    while (*length != 0u && (**start == ' ' || **start == '\t')) {
        (*start)++;
        (*length)--;
    }
    while (*length != 0u &&
           ((*start)[*length - 1u] == ' ' || (*start)[*length - 1u] == '\t'))
        (*length)--;
}

static int knurl_proxy_ws_extension_tail(const char *cursor, const char *end)
{
    const char *start;

    while (cursor < end) {
        while (cursor < end && (*cursor == ' ' || *cursor == '\t'))
            ++cursor;
        if (cursor == end)
            return 1;
        if (*cursor++ != ';')
            return 0;
        while (cursor < end && (*cursor == ' ' || *cursor == '\t'))
            ++cursor;
        start = cursor;
        while (cursor < end && knurl_proxy_ws_token((unsigned char)*cursor))
            ++cursor;
        if (cursor == start)
            return 0;
        while (cursor < end && (*cursor == ' ' || *cursor == '\t'))
            ++cursor;
        if (cursor < end && *cursor == '=') {
            ++cursor;
            while (cursor < end && (*cursor == ' ' || *cursor == '\t'))
                ++cursor;
            if (cursor < end && *cursor == '"') {
                ++cursor;
                for (;;) {
                    if (cursor == end)
                        return 0;
                    if (*cursor == '"') {
                        ++cursor;
                        break;
                    }
                    if (*cursor == '\\') {
                        ++cursor;
                        if (cursor == end)
                            return 0;
                    }
                    if ((unsigned char)*cursor < 32u ||
                        (unsigned char)*cursor > 126u)
                        return 0;
                    ++cursor;
                }
            } else {
                start = cursor;
                while (cursor < end &&
                       knurl_proxy_ws_token((unsigned char)*cursor))
                    ++cursor;
                if (cursor == start)
                    return 0;
            }
        }
        while (cursor < end && (*cursor == ' ' || *cursor == '\t'))
            ++cursor;
        if (cursor < end && *cursor != ';')
            return 0;
    }
    return 1;
}

static const char *knurl_proxy_ws_field(const knurl_proxy_headers *headers,
                                        const char *name, size_t *count)
{
    const char *value;
    size_t i;

    value = NULL;
    *count = 0u;
    if (headers == NULL)
        return NULL;
    for (i = 0u; i < headers->count; ++i) {
        if (strcasecmp(headers->fields[i].name, name) != 0)
            continue;
        if (++(*count) == 1u)
            value = headers->fields[i].value;
    }
    return value;
}

static int knurl_proxy_ws_names(const char *value, int extensions,
                                knurl_proxy_ws_name *names, size_t *count)
{
    const char *start;
    const char *cursor;
    const char *end;
    size_t length;
    size_t i;
    int quoted;
    int escaped;

    if (value == NULL || *value == '\0')
        return 0;
    *count = 0u;
    cursor = value;
    for (;;) {
        start = cursor;
        quoted = 0;
        escaped = 0;
        while (*cursor != '\0') {
            if (extensions && quoted) {
                if (escaped)
                    escaped = 0;
                else if (*cursor == '\\')
                    escaped = 1;
                else if (*cursor == '"')
                    quoted = 0;
            } else if (extensions && *cursor == '"') {
                quoted = 1;
            } else if (*cursor == ',') {
                break;
            }
            ++cursor;
        }
        if (quoted)
            return 0;
        length = (size_t)(cursor - start);
        knurl_proxy_ws_trim(&start, &length);
        if (extensions) {
            end = start + length;
            for (i = 0u; i < length && start[i] != ';'; ++i)
                ;
            length = i;
            knurl_proxy_ws_trim(&start, &length);
            if (!knurl_proxy_ws_extension_tail(start + i, end))
                return 0;
        }
        if (length == 0u || *count >= KNURL_PROXY_HEADER_COUNT_LIMIT)
            return 0;
        for (i = 0u; i < length; ++i) {
            if (!knurl_proxy_ws_token((unsigned char)start[i]))
                return 0;
        }
        names[*count].text = start;
        names[*count].length = length;
        (*count)++;
        if (*cursor == '\0')
            break;
        ++cursor;
        if (*cursor == '\0')
            return 0;
    }
    return 1;
}

static int knurl_proxy_ws_contains(const knurl_proxy_ws_name *names,
                                   size_t count, const knurl_proxy_ws_name *one,
                                   int ignore_case)
{
    size_t i;

    for (i = 0u; i < count; ++i) {
        if (names[i].length != one->length)
            continue;
        if (ignore_case
                ? strncasecmp(names[i].text, one->text, one->length) == 0
                : memcmp(names[i].text, one->text, one->length) == 0)
            return 1;
    }
    return 0;
}

static int knurl_proxy_ws_accept(const char *key, char output[29])
{
    unsigned char decoded[24];
    unsigned char encoded[25];
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned char material[sizeof(KNURL_PROXY_WS_GUID) + 24u];
    unsigned digest_length;

    if (key == NULL || strlen(key) != 24u || key[22] != '=' || key[23] != '=' ||
        EVP_DecodeBlock(decoded, (const unsigned char *)key, 24) != 18 ||
        EVP_EncodeBlock(encoded, decoded, 16) != 24 ||
        memcmp(encoded, key, 24u) != 0)
        return 0;
    memcpy(material, key, 24u);
    memcpy(material + 24u, KNURL_PROXY_WS_GUID,
           sizeof(KNURL_PROXY_WS_GUID) - 1u);
    if (EVP_Digest(material, 24u + sizeof(KNURL_PROXY_WS_GUID) - 1u, digest,
                   &digest_length, EVP_sha1(), NULL) != 1 ||
        digest_length != 20u ||
        EVP_EncodeBlock((unsigned char *)output, digest, digest_length) != 28)
        return 0;
    return 1;
}

int knurl_proxy_ws_request_valid(knurl_http_method method,
                                 const knurl_proxy_headers *headers,
                                 const char **reason)
{
    knurl_proxy_request_head head;
    knurl_proxy_ws_name names[KNURL_PROXY_HEADER_COUNT_LIMIT];
    const char *key;
    const char *version;
    const char *protocol;
    const char *extensions;
    char accept[29];
    size_t count;
    size_t name_count;

    if (reason != NULL)
        *reason = NULL;
    if (method != KNURL_HTTP_GET || headers == NULL ||
        knurl_proxy_request_head_parse(headers, &head, NULL) !=
            KNURL_PROXY_HEADER_OK ||
        !head.websocket_upgrade)
        return knurl_proxy_ws_fail(reason, "invalid WebSocket upgrade request");
    key = knurl_proxy_ws_field(headers, "Sec-WebSocket-Key", &count);
    if (count != 1u || !knurl_proxy_ws_accept(key, accept))
        return knurl_proxy_ws_fail(reason, "invalid WebSocket key");
    version = knurl_proxy_ws_field(headers, "Sec-WebSocket-Version", &count);
    if (count != 1u || strcmp(version, "13") != 0)
        return knurl_proxy_ws_fail(reason, "unsupported WebSocket version");
    (void)knurl_proxy_ws_field(headers, "Sec-WebSocket-Accept", &count);
    if (count != 0u)
        return knurl_proxy_ws_fail(reason, "client supplied WebSocket accept");
    protocol = knurl_proxy_ws_field(headers, "Sec-WebSocket-Protocol", &count);
    if (count > 1u ||
        (count == 1u && !knurl_proxy_ws_names(protocol, 0, names, &name_count)))
        return knurl_proxy_ws_fail(reason,
                                   "invalid WebSocket subprotocol offer");
    extensions =
        knurl_proxy_ws_field(headers, "Sec-WebSocket-Extensions", &count);
    if (count > 1u || (count == 1u && !knurl_proxy_ws_names(
                                          extensions, 1, names, &name_count)))
        return knurl_proxy_ws_fail(reason, "invalid WebSocket extension offer");
    return 1;
}

int knurl_proxy_ws_response_valid(const knurl_proxy_headers *request,
                                  unsigned status,
                                  const knurl_proxy_headers *response,
                                  const char **reason)
{
    knurl_proxy_ws_name offered[KNURL_PROXY_HEADER_COUNT_LIMIT];
    knurl_proxy_ws_name selected[KNURL_PROXY_HEADER_COUNT_LIMIT];
    knurl_proxy_ws_name connection[KNURL_PROXY_HEADER_COUNT_LIMIT];
    const char *value;
    const char *offer;
    const char *key;
    char accept[29];
    size_t count;
    size_t offered_count;
    size_t selected_count;
    size_t connection_count;
    size_t i;

    if (reason != NULL)
        *reason = NULL;
    if (status != 101u || request == NULL || response == NULL)
        return knurl_proxy_ws_fail(reason, "upstream did not upgrade");
    value = knurl_proxy_ws_field(response, "Upgrade", &count);
    if (count != 1u || strcasecmp(value, "websocket") != 0)
        return knurl_proxy_ws_fail(reason, "invalid upstream Upgrade field");
    value = knurl_proxy_ws_field(response, "Connection", &count);
    if (count != 1u ||
        !knurl_proxy_ws_names(value, 0, connection, &connection_count))
        return knurl_proxy_ws_fail(reason, "invalid upstream Connection field");
    selected[0].text = "Upgrade";
    selected[0].length = 7u;
    if (!knurl_proxy_ws_contains(connection, connection_count, selected, 1))
        return knurl_proxy_ws_fail(reason,
                                   "upstream omitted Connection: Upgrade");
    key = knurl_proxy_ws_field(request, "Sec-WebSocket-Key", &count);
    if (count != 1u || !knurl_proxy_ws_accept(key, accept))
        return knurl_proxy_ws_fail(reason, "invalid client WebSocket key");
    value = knurl_proxy_ws_field(response, "Sec-WebSocket-Accept", &count);
    if (count != 1u || strcmp(value, accept) != 0)
        return knurl_proxy_ws_fail(reason,
                                   "upstream WebSocket accept mismatch");
    value = knurl_proxy_ws_field(response, "Content-Length", &count);
    if (count != 0u ||
        knurl_proxy_ws_field(response, "Transfer-Encoding", &count) != NULL ||
        count != 0u)
        return knurl_proxy_ws_fail(reason, "upstream 101 has body framing");
    value = knurl_proxy_ws_field(response, "Sec-WebSocket-Protocol", &count);
    if (count > 1u)
        return knurl_proxy_ws_fail(reason, "duplicate upstream subprotocol");
    if (count == 1u) {
        if (!knurl_proxy_ws_names(value, 0, selected, &selected_count) ||
            selected_count != 1u)
            return knurl_proxy_ws_fail(reason, "invalid upstream subprotocol");
        offer = knurl_proxy_ws_field(request, "Sec-WebSocket-Protocol", &count);
        if (count != 1u ||
            !knurl_proxy_ws_names(offer, 0, offered, &offered_count) ||
            !knurl_proxy_ws_contains(offered, offered_count, selected, 0))
            return knurl_proxy_ws_fail(reason,
                                       "unoffered upstream subprotocol");
    }
    value = knurl_proxy_ws_field(response, "Sec-WebSocket-Extensions", &count);
    if (count > 1u)
        return knurl_proxy_ws_fail(reason, "duplicate upstream extensions");
    if (count == 1u) {
        if (!knurl_proxy_ws_names(value, 1, selected, &selected_count))
            return knurl_proxy_ws_fail(reason, "invalid upstream extensions");
        offer =
            knurl_proxy_ws_field(request, "Sec-WebSocket-Extensions", &count);
        if (count != 1u ||
            !knurl_proxy_ws_names(offer, 1, offered, &offered_count))
            return knurl_proxy_ws_fail(reason, "unoffered upstream extensions");
        for (i = 0u; i < selected_count; ++i) {
            if (!knurl_proxy_ws_contains(offered, offered_count, &selected[i],
                                         1) ||
                knurl_proxy_ws_contains(selected, i, &selected[i], 1))
                return knurl_proxy_ws_fail(reason,
                                           "unoffered or repeated extension");
        }
    }
    return 1;
}
