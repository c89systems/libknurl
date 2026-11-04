#include "channel_internal.h"
#include "response_internal.h"
#include "loop_internal.h"
#include "server_internal.h"

#include <openssl/evp.h>
#include <stdlib.h>
#include <string.h>

static int token_in(rotmos_bytes field, const char *token, int fold)
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

static int offered(rotmos_request *request, const char *name, const char *token)
{
    size_t i;
    for (i = 0; i < request->header_count; ++i)
        if (rotmos_text_equal_case(request->headers[i].name.data, name) &&
            token_in(request->headers[i].value, token, 0))
            return 1;
    return 0;
}

static int single(rotmos_request *request, const char *name, rotmos_bytes *out)
{
    size_t i;
    int count;
    count = 0;
    for (i = 0; i < request->header_count; ++i)
        if (rotmos_text_equal_case(request->headers[i].name.data, name)) {
            *out = request->headers[i].value;
            ++count;
        }
    return count == 1;
}

static int connection_upgrade(rotmos_request *request)
{
    size_t i;
    int upgrade;
    upgrade = 0;
    for (i = 0; i < request->header_count; ++i) {
        if (!rotmos_text_equal_case(request->headers[i].name.data,
                                    "connection"))
            continue;
        if (token_in(request->headers[i].value, "close", 1))
            return 0;
        if (token_in(request->headers[i].value, "upgrade", 1))
            upgrade = 1;
    }
    return upgrade;
}

int rotmos_websocket_accept_key(rotmos_request *request, char *accept)
{
    rotmos_bytes key;
    rotmos_bytes version;
    rotmos_bytes upgrade;
    unsigned char decoded[18];
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned char canonical[25];
    char joined[61];
    unsigned int length;
    size_t i;
    unsigned char byte;
    if (request->method != ROTMOS_HTTP_GET || request->version.length != 8 ||
        memcmp(request->version.data, "HTTP/1.1", 8) != 0 ||
        request->has_coding ||
        (request->has_length &&
         (request->content_length.high || request->content_length.low)) ||
        !single(request, "sec-websocket-key", &key) || key.length != 24 ||
        !single(request, "sec-websocket-version", &version) ||
        version.length != 2 || memcmp(version.data, "13", 2) != 0 ||
        !single(request, "upgrade", &upgrade) ||
        !rotmos_text_equal_case(upgrade.data, "websocket") ||
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

rotmos_status rotmos_channel_upgrade(rotmos_channel *channel,
                                     const rotmos_header *headers, size_t count,
                                     rotmos_error *error)
{
    rotmos_request *request;
    rotmos_reply *copy;
    const rotmos_header *candidate;
    rotmos_status status;
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
    rotmos_bytes field;
    if (channel == NULL || (count != 0 && headers == NULL))
        return rotmos_fail(error, ROTMOS_INVALID, "channel_upgrade",
                           "provide a live channel and handshake fields");
    request = channel->request;
    status = rotmos_loop_owner(request->server->loop, error);
    if (status != ROTMOS_OK)
        return status;
    if (channel->phase != ROTMOS_CHANNEL_HTTP_INPUT || channel->input_complete)
        return rotmos_fail(
            error, ROTMOS_STATE, "channel_upgrade",
            "channel has already completed or upgraded HTTP input");
    if (!rotmos_websocket_accept_key(request, accept))
        return rotmos_fail(error, ROTMOS_INVALID, "channel_upgrade",
                           "request is not a valid WebSocket handshake");
    existing = request->response->headers->header_count;
    if (count > (size_t)-1 - existing)
        return rotmos_fail(error, ROTMOS_LIMIT, "channel_upgrade",
                           "handshake field count exceeds its finite bound");
    status = rotmos_reply_new(&copy, error);
    if (status != ROTMOS_OK)
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
            status = rotmos_fail(error, ROTMOS_LIMIT, "channel_upgrade",
                                 "handshake exceeds its finite header bound");
            break;
        }
        total += candidate->name.length + candidate->value.length + 4;
        if (candidate->name.data == NULL || candidate->value.data == NULL ||
            memchr(candidate->name.data, '\0', candidate->name.length) !=
                NULL ||
            memchr(candidate->value.data, '\0', candidate->value.length) !=
                NULL) {
            status = rotmos_fail(error, ROTMOS_INVALID, "channel_upgrade",
                                 "invalid handshake text");
            break;
        }
        name = malloc(candidate->name.length + 1);
        value = malloc(candidate->value.length + 1);
        if (name == NULL || value == NULL) {
            free(name);
            free(value);
            status = rotmos_fail(error, ROTMOS_NOMEM, "channel_upgrade",
                                 "cannot copy handshake fields");
            break;
        }
        memcpy(name, candidate->name.data, candidate->name.length);
        name[candidate->name.length] = '\0';
        memcpy(value, candidate->value.data, candidate->value.length);
        value[candidate->value.length] = '\0';
        if (!rotmos_header_name_valid(name) ||
            !rotmos_header_value_valid(value))
            status = ROTMOS_INVALID;
        else if (rotmos_text_equal_case(name, "upgrade")) {
            ++upgrades;
            if (!rotmos_text_equal_case(value, "websocket"))
                status = ROTMOS_INVALID;
        } else if (rotmos_text_equal_case(name, "connection")) {
            ++connections;
            field = rotmos_view(value);
            if (token_in(field, "upgrade", 1))
                connection_upgrades = 1;
            if (token_in(field, "close", 1))
                status = ROTMOS_INVALID;
        } else if (rotmos_text_equal_case(name, "sec-websocket-accept")) {
            ++accepts;
            if (strlen(value) != 28 || memcmp(value, accept, 28) != 0)
                status = ROTMOS_INVALID;
        } else if (rotmos_text_equal_case(name, "sec-websocket-protocol")) {
            ++protocols;
            if (!rotmos_header_name_valid(value) ||
                !offered(request, "sec-websocket-protocol", value))
                status = ROTMOS_INVALID;
        } else if (rotmos_text_equal_case(name, "sec-websocket-extensions")) {
            ++extensions;
            if (value[0] == '\0' ||
                rotmos_request_header_first(request, "sec-websocket-extensions",
                                            &field, NULL) != ROTMOS_OK ||
                field.length == 0)
                status = ROTMOS_INVALID;
        } else if (rotmos_text_equal_case(name, "content-length") ||
                   rotmos_text_equal_case(name, "transfer-encoding") ||
                   rotmos_text_equal_case(name, "keep-alive") ||
                   rotmos_text_equal_case(name, "trailer") ||
                   rotmos_text_equal_case(name, "sec-websocket-key") ||
                   rotmos_text_equal_case(name, "sec-websocket-version")) {
            status = ROTMOS_INVALID;
        }
        if (status == ROTMOS_OK)
            status = rotmos_reply_header_add(copy, name, value, error);
        free(name);
        free(value);
        if (status != ROTMOS_OK)
            break;
    }
    if (status == ROTMOS_OK &&
        (upgrades != 1 || connections == 0 || !connection_upgrades ||
         accepts != 1 || protocols > 1 || extensions > 1))
        status = ROTMOS_INVALID;
    if (status == ROTMOS_OK)
        status = rotmos_response_upgrade_headers(
            request->response, copy->headers, copy->header_count, error);
    (void)rotmos_reply_destroy(copy, NULL);
    if (status != ROTMOS_OK) {
        if (error == NULL || error->status != status)
            return rotmos_fail(
                error, status, "channel_upgrade",
                "invalid or conflicting upstream WebSocket handshake");
        return status;
    }
    channel->phase = ROTMOS_CHANNEL_TUNNEL;
    request->outcome.body_complete = 1;
    request->outcome.body_cause = ROTMOS_OK;
    channel->ops.completed(channel);
    channel->ops.interest(channel);
    rotmos_channel_schedule(channel);
    rotmos_error_clear(error);
    return ROTMOS_OK;
}
