#include "channel_internal.h"
#include "response_internal.h"
#include "loop_internal.h"
#include "server_internal.h"

#include <openssl/evp.h>
#include <stdlib.h>
#include <string.h>

static int token_in(knurl_bytes field, const char *token, int fold)
{
    const unsigned char *text;
    size_t at;
    size_t start;
    size_t end;
    size_t i;
    size_t length;
    unsigned char a;
    unsigned char b;
    text = field.data;
    length = strlen(token);
    at = 0;
    while (at < field.length) {
        while (at < field.length &&
               (text[at] == ',' || text[at] == ' ' || text[at] == '\t'))
            ++at;
        start = at;
        while (at < field.length && text[at] != ',')
            ++at;
        end = at;
        while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t'))
            --end;
        if (end - start != length)
            continue;
        for (i = 0; i < length; ++i) {
            a = text[start + i];
            b = (unsigned char)token[i];
            if (fold) {
                if (a >= 'A' && a <= 'Z')
                    a = (unsigned char)(a + 'a' - 'A');
                if (b >= 'A' && b <= 'Z')
                    b = (unsigned char)(b + 'a' - 'A');
            }
            if (a != b)
                break;
        }
        if (i == length)
            return 1;
    }
    return 0;
}

static int offered(knurl_request *request, const char *name, const char *token)
{
    size_t i;
    for (i = 0; i < request->header_count; ++i)
        if (knurl_text_equal_case(request->headers[i].name.data, name) &&
            token_in(request->headers[i].value, token, 0))
            return 1;
    return 0;
}

static int single(knurl_request *request, const char *name, knurl_bytes *out)
{
    size_t i;
    int count;
    out->data = NULL;
    out->length = 0;
    count = 0;
    for (i = 0; i < request->header_count; ++i)
        if (knurl_text_equal_case(request->headers[i].name.data, name)) {
            *out = request->headers[i].value;
            ++count;
        }
    return count == 1;
}

static int connection_upgrade(knurl_request *request)
{
    size_t i;
    int upgrade;
    upgrade = 0;
    for (i = 0; i < request->header_count; ++i) {
        if (!knurl_text_equal_case(request->headers[i].name.data, "connection"))
            continue;
        if (token_in(request->headers[i].value, "close", 1))
            return 0;
        if (token_in(request->headers[i].value, "upgrade", 1))
            upgrade = 1;
    }
    return upgrade;
}

int knurl_websocket_accept_key(knurl_request *request, char *accept)
{
    knurl_bytes key;
    knurl_bytes version;
    knurl_bytes upgrade;
    unsigned char decoded[18];
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned char canonical[25];
    char joined[61];
    unsigned int length;
    size_t i;
    unsigned char byte;
    if (request->method != KNURL_HTTP_GET || request->version.length != 8 ||
        memcmp(request->version.data, "HTTP/1.1", 8) != 0 ||
        request->has_coding ||
        (request->has_length &&
         (request->content_length.high || request->content_length.low)) ||
        !single(request, "sec-websocket-key", &key) || key.length != 24 ||
        !single(request, "sec-websocket-version", &version) ||
        version.length != 2 || memcmp(version.data, "13", 2) != 0 ||
        !single(request, "upgrade", &upgrade) ||
        !knurl_text_equal_case(upgrade.data, "websocket") ||
        !connection_upgrade(request))
        return 0;
    for (i = 0; i < 22; ++i) {
        byte = ((const unsigned char *)key.data)[i];
        if (!((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
              (byte >= '0' && byte <= '9') || byte == '+' || byte == '/'))
            return 0;
    }
    if (memcmp((const char *)key.data + 22, "==", 2) != 0 ||
        EVP_DecodeBlock(decoded, key.data, 24) != 18 ||
        EVP_EncodeBlock(canonical, decoded, 16) != 24 ||
        memcmp(canonical, key.data, 24) != 0)
        return 0;
    memcpy(joined, key.data, 24);
    memcpy(joined + 24, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", 36);
    joined[60] = '\0';
    if (!EVP_Digest(joined, 60, digest, &length, EVP_sha1(), NULL) ||
        length != 20)
        return 0;
    return EVP_EncodeBlock((unsigned char *)accept, digest, 20) == 28;
}

knurl_status knurl_channel_upgrade(knurl_channel *channel,
                                   const knurl_header *headers, size_t count,
                                   knurl_error *error)
{
    knurl_request *request;
    knurl_reply *copy;
    const knurl_header *candidate;
    knurl_status status;
    char accept[29];
    char *name;
    char *value;
    size_t i;
    size_t existing;
    size_t total;
    int upgrades;
    int connections;
    int connection_upgrades;
    int accepts;
    int protocols;
    int extensions;
    knurl_bytes field;
    if (channel == NULL || (count != 0 && headers == NULL))
        return knurl_fail(error, KNURL_INVALID, "channel_upgrade",
                          "provide a live channel and handshake fields");
    request = channel->request;
    status = knurl_loop_owner(request->server->loop, error);
    if (status != KNURL_OK)
        return status;
    if (channel->phase != KNURL_CHANNEL_HTTP_INPUT || channel->input_complete)
        return knurl_fail(
            error, KNURL_STATE, "channel_upgrade",
            "channel has already completed or upgraded HTTP input");
    if (!knurl_websocket_accept_key(request, accept))
        return knurl_fail(error, KNURL_INVALID, "channel_upgrade",
                          "request is not a valid WebSocket handshake");
    existing = request->response->headers->header_count;
    if (count > (size_t)-1 - existing)
        return knurl_fail(error, KNURL_LIMIT, "channel_upgrade",
                          "handshake field count exceeds its finite bound");
    status = knurl_reply_new(&copy, error);
    if (status != KNURL_OK)
        return status;
    total = 0;
    connection_upgrades = 0;
    upgrades = connections = accepts = protocols = extensions = 0;
    for (i = 0; i < existing + count; ++i) {
        candidate = i < existing ? &request->response->headers->headers[i]
                                 : &headers[i - existing];
        if (candidate->name.length > request->response->header_bound ||
            candidate->value.length > request->response->header_bound ||
            candidate->name.length >
                request->response->header_bound - candidate->value.length ||
            candidate->name.length + candidate->value.length + 4 >
                request->response->header_bound - total) {
            status = knurl_fail(error, KNURL_LIMIT, "channel_upgrade",
                                "handshake exceeds its finite header bound");
            break;
        }
        total += candidate->name.length + candidate->value.length + 4;
        if (candidate->name.data == NULL || candidate->value.data == NULL ||
            memchr(candidate->name.data, '\0', candidate->name.length) !=
                NULL ||
            memchr(candidate->value.data, '\0', candidate->value.length) !=
                NULL) {
            status = knurl_fail(error, KNURL_INVALID, "channel_upgrade",
                                "invalid handshake text");
            break;
        }
        name = malloc(candidate->name.length + 1);
        value = malloc(candidate->value.length + 1);
        if (name == NULL || value == NULL) {
            free(name);
            free(value);
            status = knurl_fail(error, KNURL_NOMEM, "channel_upgrade",
                                "cannot copy handshake fields");
            break;
        }
        memcpy(name, candidate->name.data, candidate->name.length);
        name[candidate->name.length] = '\0';
        memcpy(value, candidate->value.data, candidate->value.length);
        value[candidate->value.length] = '\0';
        if (!knurl_header_name_valid(name) || !knurl_header_value_valid(value))
            status = KNURL_INVALID;
        else if (knurl_text_equal_case(name, "upgrade")) {
            ++upgrades;
            if (!knurl_text_equal_case(value, "websocket"))
                status = KNURL_INVALID;
        } else if (knurl_text_equal_case(name, "connection")) {
            ++connections;
            field = knurl_view(value);
            if (token_in(field, "upgrade", 1))
                connection_upgrades = 1;
            if (token_in(field, "close", 1))
                status = KNURL_INVALID;
        } else if (knurl_text_equal_case(name, "sec-websocket-accept")) {
            ++accepts;
            if (strlen(value) != 28 || memcmp(value, accept, 28) != 0)
                status = KNURL_INVALID;
        } else if (knurl_text_equal_case(name, "sec-websocket-protocol")) {
            ++protocols;
            if (!knurl_header_name_valid(value) ||
                !offered(request, "sec-websocket-protocol", value))
                status = KNURL_INVALID;
        } else if (knurl_text_equal_case(name, "sec-websocket-extensions")) {
            ++extensions;
            if (value[0] == '\0' ||
                knurl_request_header_first(request, "sec-websocket-extensions",
                                           &field, NULL) != KNURL_OK ||
                field.length == 0)
                status = KNURL_INVALID;
        } else if (knurl_text_equal_case(name, "content-length") ||
                   knurl_text_equal_case(name, "transfer-encoding") ||
                   knurl_text_equal_case(name, "keep-alive") ||
                   knurl_text_equal_case(name, "trailer") ||
                   knurl_text_equal_case(name, "sec-websocket-key") ||
                   knurl_text_equal_case(name, "sec-websocket-version")) {
            status = KNURL_INVALID;
        }
        if (status == KNURL_OK)
            status = knurl_reply_header_add(copy, name, value, error);
        free(name);
        free(value);
        if (status != KNURL_OK)
            break;
    }
    if (status == KNURL_OK &&
        (upgrades != 1 || connections == 0 || !connection_upgrades ||
         accepts != 1 || protocols > 1 || extensions > 1))
        status = KNURL_INVALID;
    if (status == KNURL_OK)
        status = knurl_response_upgrade_headers(
            request->response, copy->headers, copy->header_count, error);
    (void)knurl_reply_destroy(copy, NULL);
    if (status != KNURL_OK) {
        if (error == NULL || error->status != status)
            return knurl_fail(
                error, status, "channel_upgrade",
                "invalid or conflicting upstream WebSocket handshake");
        return status;
    }
    channel->phase = KNURL_CHANNEL_TUNNEL;
    request->outcome.body_complete = 1;
    request->outcome.body_cause = KNURL_OK;
    channel->ops.completed(channel);
    channel->ops.interest(channel);
    knurl_channel_schedule(channel);
    knurl_error_clear(error);
    return KNURL_OK;
}
