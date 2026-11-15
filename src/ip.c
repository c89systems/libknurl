#include "ip.h"
#include <string.h>
int knurl_ip_compare(const knurl_mmdb_address *a, const knurl_mmdb_address *b)
{
    if (a->size != b->size)
        return a->size < b->size ? -1 : 1;
    return memcmp(a->bytes, b->bytes, a->size);
}
int knurl_ip_normalize(const knurl_mmdb_address *input, knurl_mmdb_address *out)
{
    static const unsigned char mapped[12] = {0, 0, 0, 0, 0,   0,
                                             0, 0, 0, 0, 255, 255};
    if (input == NULL || (input->size != 4 && input->size != 16))
        return 0;
    memset(out, 0, sizeof(*out));
    out->size = input->size;
    if (input->size == 16 && memcmp(input->bytes, mapped, 12) == 0) {
        out->size = 4;
        memcpy(out->bytes, input->bytes + 12, 4);
    } else
        memcpy(out->bytes, input->bytes, input->size);
    return 1;
}
int knurl_ip_network_parse(const char *text, struct knurl_ip_network *out)
{
    knurl_mmdb_address parsed;
    char value[64];
    const char *slash;
    size_t length;
    unsigned int bits = 0, maximum;
    if (text == NULL || (length = strlen(text)) >= sizeof(value))
        return 0;
    slash = strchr(text, '/');
    memcpy(value, text, length + 1);
    if (slash != NULL)
        value[slash - text] = 0;
    if (knurl_mmdb_address_parse(value, &parsed, NULL) != KNURL_OK)
        return 0;
    maximum = parsed.size * 8;
    if (slash == NULL)
        bits = maximum;
    else {
        ++slash;
        if (*slash == 0)
            return 0;
        while (*slash) {
            if (*slash < '0' || *slash > '9' || bits > 128)
                return 0;
            bits = bits * 10 + (unsigned int)(*slash++ - '0');
        }
        if (bits > maximum)
            return 0;
    }
    (void)knurl_ip_normalize(&parsed, &out->address);
    if (out->address.size != parsed.size) {
        if (bits < 96)
            return 0;
        bits -= 96;
    }
    out->bits = bits;
    return 1;
}
int knurl_ip_contains(const struct knurl_ip_network *network,
                      const knurl_mmdb_address *address)
{
    unsigned int bytes = network->bits / 8, rest = network->bits % 8;
    return network->address.size == address->size &&
           memcmp(network->address.bytes, address->bytes, bytes) == 0 &&
           (!rest || ((network->address.bytes[bytes] ^ address->bytes[bytes]) &
                      (255u << (8 - rest))) == 0);
}
int knurl_ip_hostname(const char *text)
{
    size_t i, label = 0, length;
    int alpha = 0;
    unsigned char c;
    if (text == NULL || (length = strlen(text)) == 0 ||
        length > (text[length - 1] == '.' ? 254u : 253u))
        return 0;
    for (i = 0; i < length; ++i) {
        c = (unsigned char)text[i];
        if (c == '.') {
            if (label == 0 || text[i - 1] == '-')
                return 0;
            label = 0;
        } else {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-') ||
                (label == 0 && c == '-') || ++label > 63)
                return 0;
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
                alpha = 1;
        }
    }
    return alpha && (label == 0 || text[length - 1] != '-');
}
