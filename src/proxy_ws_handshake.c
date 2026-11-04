#include "proxy_ws_handshake.h"

#include <openssl/evp.h>
#include <string.h>
#include <strings.h>

#define ROTMOS_PROXY_WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

typedef struct rotmos_proxy_ws_name {
    const char *text;
    size_t length;
} rotmos_proxy_ws_name;

static int rotmos_proxy_ws_fail(const char **reason, const char *message)
{
    if (reason != NULL)
        *reason = message;
    return 0;
}

static int rotmos_proxy_ws_token(unsigned char value)
{
    if ((value >= '0' && value <= '9') || (value >= 'A' && value <= 'Z') ||
        (value >= 'a' && value <= 'z'))
        return 1;
    return strchr("!#$%&'*+-.^_`|~", (int)value) != NULL;
}

static void rotmos_proxy_ws_trim(const char **start, size_t *length)
{
    while (*length != 0u && (**start == ' ' || **start == '\t')) {
        (*start)++;
        (*length)--;
    }
    while (*length != 0u &&
           ((*start)[*length - 1u] == ' ' || (*start)[*length - 1u] == '\t'))
        (*length)--;
}

static int rotmos_proxy_ws_extension_tail(const char *cursor, const char *end)
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
        while (cursor < end && rotmos_proxy_ws_token((unsigned char)*cursor))
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
                       rotmos_proxy_ws_token((unsigned char)*cursor))
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

static const char *rotmos_proxy_ws_field(const rotmos_proxy_headers *headers,
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

static int rotmos_proxy_ws_names(const char *value, int extensions,
                                 rotmos_proxy_ws_name *names, size_t *count)
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
        rotmos_proxy_ws_trim(&start, &length);
        if (extensions) {
            end = start + length;
            for (i = 0u; i < length && start[i] != ';'; ++i)
                ;
            length = i;
            rotmos_proxy_ws_trim(&start, &length);
            if (!rotmos_proxy_ws_extension_tail(start + i, end))
                return 0;
        }
        if (length == 0u || *count >= ROTMOS_PROXY_HEADER_COUNT_LIMIT)
            return 0;
        for (i = 0u; i < length; ++i) {
            if (!rotmos_proxy_ws_token((unsigned char)start[i]))
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

static int rotmos_proxy_ws_contains(const rotmos_proxy_ws_name *names,
                                    size_t count,
                                    const rotmos_proxy_ws_name *one,
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

static int rotmos_proxy_ws_accept(const char *key, char output[29])
{
    unsigned char decoded[24];
    unsigned char encoded[25];
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned char material[sizeof(ROTMOS_PROXY_WS_GUID) + 24u];
    unsigned digest_length;

    if (key == NULL || strlen(key) != 24u || key[22] != '=' || key[23] != '=' ||
        EVP_DecodeBlock(decoded, (const unsigned char *)key, 24) != 18 ||
        EVP_EncodeBlock(encoded, decoded, 16) != 24 ||
        memcmp(encoded, key, 24u) != 0)
        return 0;
    memcpy(material, key, 24u);
    memcpy(material + 24u, ROTMOS_PROXY_WS_GUID,
           sizeof(ROTMOS_PROXY_WS_GUID) - 1u);
    if (EVP_Digest(material, 24u + sizeof(ROTMOS_PROXY_WS_GUID) - 1u, digest,
                   &digest_length, EVP_sha1(), NULL) != 1 ||
        digest_length != 20u ||
        EVP_EncodeBlock((unsigned char *)output, digest, digest_length) != 28)
        return 0;
    return 1;
}

int rotmos_proxy_ws_request_valid(rotmos_http_method method,
                                  const rotmos_proxy_headers *headers,
                                  const char **reason)
{
    rotmos_proxy_request_head head;
    rotmos_proxy_ws_name names[ROTMOS_PROXY_HEADER_COUNT_LIMIT];
    const char *key;
    const char *version;
    const char *protocol;
    const char *extensions;
    char accept[29];
    size_t count;
    size_t name_count;

    if (reason != NULL)
        *reason = NULL;
    if (method != ROTMOS_HTTP_GET || headers == NULL ||
        rotmos_proxy_request_head_parse(headers, &head, NULL) !=
            ROTMOS_PROXY_HEADER_OK ||
        !head.websocket_upgrade)
        return rotmos_proxy_ws_fail(reason,
                                    "invalid WebSocket upgrade request");
    key = rotmos_proxy_ws_field(headers, "Sec-WebSocket-Key", &count);
    if (count != 1u || !rotmos_proxy_ws_accept(key, accept))
        return rotmos_proxy_ws_fail(reason, "invalid WebSocket key");
    version = rotmos_proxy_ws_field(headers, "Sec-WebSocket-Version", &count);
    if (count != 1u || strcmp(version, "13") != 0)
        return rotmos_proxy_ws_fail(reason, "unsupported WebSocket version");
    (void)rotmos_proxy_ws_field(headers, "Sec-WebSocket-Accept", &count);
    if (count != 0u)
        return rotmos_proxy_ws_fail(reason, "client supplied WebSocket accept");
    protocol = rotmos_proxy_ws_field(headers, "Sec-WebSocket-Protocol", &count);
    if (count > 1u || (count == 1u &&
                       !rotmos_proxy_ws_names(protocol, 0, names, &name_count)))
        return rotmos_proxy_ws_fail(reason,
                                    "invalid WebSocket subprotocol offer");
    extensions =
        rotmos_proxy_ws_field(headers, "Sec-WebSocket-Extensions", &count);
    if (count > 1u || (count == 1u && !rotmos_proxy_ws_names(
                                          extensions, 1, names, &name_count)))
        return rotmos_proxy_ws_fail(reason,
                                    "invalid WebSocket extension offer");
    return 1;
}

int rotmos_proxy_ws_response_valid(const rotmos_proxy_headers *request,
                                   unsigned status,
                                   const rotmos_proxy_headers *response,
                                   const char **reason)
{
    rotmos_proxy_ws_name offered[ROTMOS_PROXY_HEADER_COUNT_LIMIT];
    rotmos_proxy_ws_name selected[ROTMOS_PROXY_HEADER_COUNT_LIMIT];
    rotmos_proxy_ws_name connection[ROTMOS_PROXY_HEADER_COUNT_LIMIT];
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
        return rotmos_proxy_ws_fail(reason, "upstream did not upgrade");
    value = rotmos_proxy_ws_field(response, "Upgrade", &count);
    if (count != 1u || strcasecmp(value, "websocket") != 0)
        return rotmos_proxy_ws_fail(reason, "invalid upstream Upgrade field");
    value = rotmos_proxy_ws_field(response, "Connection", &count);
    if (count != 1u ||
        !rotmos_proxy_ws_names(value, 0, connection, &connection_count))
        return rotmos_proxy_ws_fail(reason,
                                    "invalid upstream Connection field");
    selected[0].text = "Upgrade";
    selected[0].length = 7u;
    if (!rotmos_proxy_ws_contains(connection, connection_count, selected, 1))
        return rotmos_proxy_ws_fail(reason,
                                    "upstream omitted Connection: Upgrade");
    key = rotmos_proxy_ws_field(request, "Sec-WebSocket-Key", &count);
    if (count != 1u || !rotmos_proxy_ws_accept(key, accept))
        return rotmos_proxy_ws_fail(reason, "invalid client WebSocket key");
    value = rotmos_proxy_ws_field(response, "Sec-WebSocket-Accept", &count);
    if (count != 1u || strcmp(value, accept) != 0)
        return rotmos_proxy_ws_fail(reason,
                                    "upstream WebSocket accept mismatch");
    value = rotmos_proxy_ws_field(response, "Content-Length", &count);
    if (count != 0u ||
        rotmos_proxy_ws_field(response, "Transfer-Encoding", &count) != NULL ||
        count != 0u)
        return rotmos_proxy_ws_fail(reason, "upstream 101 has body framing");
    value = rotmos_proxy_ws_field(response, "Sec-WebSocket-Protocol", &count);
    if (count > 1u)
        return rotmos_proxy_ws_fail(reason, "duplicate upstream subprotocol");
    if (count == 1u) {
        if (!rotmos_proxy_ws_names(value, 0, selected, &selected_count) ||
            selected_count != 1u)
            return rotmos_proxy_ws_fail(reason, "invalid upstream subprotocol");
        offer =
            rotmos_proxy_ws_field(request, "Sec-WebSocket-Protocol", &count);
        if (count != 1u ||
            !rotmos_proxy_ws_names(offer, 0, offered, &offered_count) ||
            !rotmos_proxy_ws_contains(offered, offered_count, selected, 0))
            return rotmos_proxy_ws_fail(reason,
                                        "unoffered upstream subprotocol");
    }
    value = rotmos_proxy_ws_field(response, "Sec-WebSocket-Extensions", &count);
    if (count > 1u)
        return rotmos_proxy_ws_fail(reason, "duplicate upstream extensions");
    if (count == 1u) {
        if (!rotmos_proxy_ws_names(value, 1, selected, &selected_count))
            return rotmos_proxy_ws_fail(reason, "invalid upstream extensions");
        offer =
            rotmos_proxy_ws_field(request, "Sec-WebSocket-Extensions", &count);
        if (count != 1u ||
            !rotmos_proxy_ws_names(offer, 1, offered, &offered_count))
            return rotmos_proxy_ws_fail(reason,
                                        "unoffered upstream extensions");
        for (i = 0u; i < selected_count; ++i) {
            if (!rotmos_proxy_ws_contains(offered, offered_count, &selected[i],
                                          1) ||
                rotmos_proxy_ws_contains(selected, i, &selected[i], 1))
                return rotmos_proxy_ws_fail(reason,
                                            "unoffered or repeated extension");
        }
    }
    return 1;
}
