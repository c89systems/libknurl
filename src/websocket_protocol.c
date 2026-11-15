#include "websocket_internal.h"

int knurl_websocket_message_check(struct knurl_websocket_message *state,
                                  knurl_websocket_opcode opcode, int final,
                                  const unsigned char *bytes, size_t length,
                                  size_t maximum)
{
    size_t i;
    unsigned long c;
    if (opcode == KNURL_WEBSOCKET_CONTINUATION) {
        if (state->kind == 0)
            return 1002;
    } else if (opcode == KNURL_WEBSOCKET_TEXT ||
               opcode == KNURL_WEBSOCKET_BINARY) {
        if (state->kind != 0)
            return 1002;
        state->kind = opcode;
    } else
        return 1002;
    if (state->bytes > maximum || length > maximum - state->bytes)
        return 1009;
    state->bytes += length;
    if (state->kind == KNURL_WEBSOCKET_TEXT) {
        for (i = 0; i < length; ++i) {
            c = bytes[i];
            if (state->need == 0) {
                if (c < 128)
                    continue;
                if (c >= 194 && c <= 223) {
                    state->need = 1;
                    state->minimum = 128;
                    state->value = c & 31;
                } else if (c >= 224 && c <= 239) {
                    state->need = 2;
                    state->minimum = 2048;
                    state->value = c & 15;
                } else if (c >= 240 && c <= 244) {
                    state->need = 3;
                    state->minimum = 65536;
                    state->value = c & 7;
                } else
                    return 1007;
            } else {
                if ((c & 192) != 128)
                    return 1007;
                state->value = (state->value << 6) | (c & 63);
                if (--state->need == 0 &&
                    (state->value < state->minimum ||
                     state->value > 0x10ffffUL ||
                     (state->value >= 0xd800UL && state->value <= 0xdfffUL)))
                    return 1007;
            }
        }
        if (final && state->need != 0)
            return 1007;
    }
    if (final) {
        state->kind = 0;
        state->bytes = 0;
    }
    return 0;
}
int knurl_websocket_close_check(const unsigned char *bytes, size_t length)
{
    unsigned short code;
    if (length == 1 || length > 125)
        return 1002;
    if (length == 0)
        return 0;
    code = (unsigned short)(bytes[0] * 256 + bytes[1]);
    if (!knurl_websocket_code(code))
        return 1002;
    return knurl_websocket_utf8(bytes + 2, length - 2) ? 0 : 1007;
}
