#include "mmdb_decode.h"
#include <string.h>

int knurl_mmdb_read(const struct knurl_mmdb_section *section, size_t at,
                    void *out, size_t length)
{
    if (at > section->size || length > section->size - at)
        return 0;
    return knurl_mmdb_cache_read(section->cache, section->base + at, out,
                                 length);
}
int knurl_mmdb_equal(const struct knurl_mmdb_section *section, size_t at,
                     const void *expected, size_t length)
{
    unsigned char bytes[64];
    const unsigned char *want = (const unsigned char *)expected;
    size_t count;
    while (length != 0) {
        count = length < sizeof(bytes) ? length : sizeof(bytes);
        if (!knurl_mmdb_read(section, at, bytes, count) ||
            memcmp(bytes, want, count) != 0)
            return 0;
        at += count;
        want += count;
        length -= count;
    }
    return 1;
}
int knurl_mmdb_same(const struct knurl_mmdb_section *section, size_t a,
                    size_t b, size_t length)
{
    unsigned char bytes[64];
    size_t count;
    while (length != 0) {
        count = length < sizeof(bytes) ? length : sizeof(bytes);
        if (!knurl_mmdb_read(section, a, bytes, count) ||
            !knurl_mmdb_equal(section, b, bytes, count))
            return 0;
        a += count;
        b += count;
        length -= count;
    }
    return 1;
}
static int octet(const struct knurl_mmdb_section *section, size_t at,
                 unsigned int *out)
{
    unsigned char byte;
    if (!knurl_mmdb_read(section, at, &byte, 1))
        return 0;
    *out = byte;
    return 1;
}

static int raw(const struct knurl_mmdb_section *section, size_t at,
               struct knurl_mmdb_value *value, unsigned long *budget)
{
    unsigned int control, type, width, i, byte;
    unsigned long size, pointer;
    if (*budget == 0 || at >= section->size)
        return 0;
    --*budget;
    if (!octet(section, at++, &control))
        return 0;
    type = control >> 5;
    if (type == 0) {
        if (at >= section->size)
            return 0;
        if (!octet(section, at++, &type))
            return 0;
        type += 7;
    }
    size = control & 31;
    value->type = type;
    if (type == 1) {
        width = ((control >> 3) & 3) + 1;
        if (width > section->size - at)
            return 0;
        pointer = width == 4 ? 0 : control & 7;
        for (i = 0; i < width; ++i) {
            if (!octet(section, at++, &byte))
                return 0;
            pointer = (pointer << 8) | byte;
        }
        if (width == 2)
            pointer += 2048UL;
        if (width == 3)
            pointer += 526336UL;
        if (pointer >= section->size)
            return 0;
        value->payload = (size_t)pointer;
        value->next = at;
        value->size = 0;
        return 1;
    }
    if (size >= 29) {
        width = (unsigned int)size - 28;
        if (width > section->size - at)
            return 0;
        size = 0;
        for (i = 0; i < width; ++i) {
            if (!octet(section, at++, &byte))
                return 0;
            size = (size << 8) | byte;
        }
        size += width == 1 ? 29UL : width == 2 ? 285UL : 65821UL;
    }
    value->size = size;
    value->payload = at;
    if (type == 7 || type == 11) {
        value->next = at;
        return 1;
    }
    if (type == 14) {
        value->next = at;
        return size <= 1;
    }
    if (type < 2 || type > 15 || type == 12 || type == 13 ||
        (type == 3 && size != 8) || (type == 15 && size != 4) ||
        (type == 5 && size > 2) || ((type == 6 || type == 8) && size > 4) ||
        (type == 9 && size > 8) || (type == 10 && size > 16) ||
        size > section->size - at)
        return 0;
    value->next = at + (size_t)size;
    return 1;
}
int knurl_mmdb_decode(const struct knurl_mmdb_section *section, size_t at,
                      struct knurl_mmdb_value *value, unsigned long *budget,
                      int follow)
{
    struct knurl_mmdb_value target;
    if (!raw(section, at, value, budget))
        return 0;
    if (follow && value->type == 1) {
        if (!raw(section, value->payload, &target, budget) || target.type == 1)
            return 0;
        target.next = value->next;
        *value = target;
    }
    return 1;
}
int knurl_mmdb_skip(const struct knurl_mmdb_section *section, size_t *at,
                    unsigned long *budget)
{
    struct knurl_mmdb_value value;
    unsigned long pending = 1, children;
    /* Linear traversal of encoded inline values. Unselected pointers are not
     * expanded, so cycles/fan-out cannot amplify work or require recursion. */
    while (pending != 0) {
        if (!raw(section, *at, &value, budget))
            return 0;
        --pending;
        children = value.type == 7    ? value.size * 2
                   : value.type == 11 ? value.size
                                      : 0;
        if (children > *budget || pending > *budget - children)
            return 0;
        pending += children;
        *at = value.next;
    }
    return 1;
}
int knurl_mmdb_select(const struct knurl_mmdb_section *section,
                      const struct knurl_mmdb_value *map, const char *name,
                      struct knurl_mmdb_value *out, unsigned long *budget)
{
    struct knurl_mmdb_value key;
    unsigned long i;
    size_t at = map->payload, length = strlen(name);
    if (map->type != 7 || map->size > *budget / 2)
        return -1;
    for (i = 0; i < map->size; ++i) {
        if (!knurl_mmdb_decode(section, at, &key, budget, 1) || key.type != 2)
            return -1;
        at = key.next;
        if (key.size == length &&
            knurl_mmdb_equal(section, key.payload, name, length))
            return knurl_mmdb_decode(section, at, out, budget, 1) ? 1 : -1;
        if (!knurl_mmdb_skip(section, &at, budget))
            return -1;
    }
    return 0;
}
unsigned long knurl_mmdb_integer(const struct knurl_mmdb_section *section,
                                 const struct knurl_mmdb_value *value)
{
    unsigned long result = 0, i;
    unsigned int byte;
    for (i = 0; i < value->size; ++i) {
        if (!octet(section, value->payload + i, &byte))
            return 0;
        result = (result << 8) | byte;
    }
    return result;
}
