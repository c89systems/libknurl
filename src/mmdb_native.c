#define _POSIX_C_SOURCE 200809L
#include "internal.h"
#include "mmdb_decode.h"
#include <knurl/mmdb.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

struct native_db {
    void *mapping;
    size_t size;
    struct knurl_mmdb_section data;
    unsigned long nodes, ipv4;
    unsigned int width, ip_version;
};
static knurl_status corrupt(knurl_error *error)
{
    return knurl_fail(error, KNURL_BACKEND, "mmdb.native",
                      "invalid or unsupported MMDB country data");
}
static void close_db(void *userdata)
{
    struct native_db *db = (struct native_db *)userdata;
    munmap(db->mapping, db->size);
    free(db);
}
static unsigned long branch(const struct native_db *db, unsigned long node,
                            unsigned int bit)
{
    const unsigned char *p =
        (const unsigned char *)db->mapping + (size_t)node * (db->width / 4);
    unsigned long result = 0;
    unsigned int i, bytes = db->width / 8;
    if (db->width == 28) {
        result = bit ? p[3] & 15 : p[3] >> 4;
        p += bit ? 4 : 0;
        bytes = 3;
    } else
        p += bit * bytes;
    for (i = 0; i < bytes; ++i)
        result = (result << 8) | p[i];
    return result;
}
static int unique_map(const struct knurl_mmdb_section *section,
                      const struct knurl_mmdb_value *map, int strings,
                      unsigned long *budget)
{
    struct key {
        size_t at, size;
    } keys[64];
    struct knurl_mmdb_value key, value;
    size_t at = map->payload;
    unsigned long i, j;
    if (map->type != 7 || map->size > 64)
        return 0;
    for (i = 0; i < map->size; ++i) {
        if (!knurl_mmdb_decode(section, at, &key, budget, 1) || key.type != 2)
            return 0;
        for (j = 0; j < i; ++j)
            if (key.size == keys[j].size &&
                memcmp(section->data + key.payload, section->data + keys[j].at,
                       key.size) == 0)
                return 0;
        keys[i].at = key.payload;
        keys[i].size = key.size;
        at = key.next;
        if (strings && (!knurl_mmdb_decode(section, at, &value, budget, 1) ||
                        value.type != 2))
            return 0;
        if (!knurl_mmdb_skip(section, &at, budget))
            return 0;
    }
    return 1;
}
static int metadata(struct native_db *db)
{
    static const unsigned char marker[] = {171, 205, 239, 'M', 'a', 'x', 'M',
                                           'i', 'n', 'd', '.', 'c', 'o', 'm'};
    static const char *names[] = {"node_count",
                                  "record_size",
                                  "ip_version",
                                  "database_type",
                                  "binary_format_major_version",
                                  "binary_format_minor_version",
                                  "build_epoch"};
    static const unsigned int types[] = {6, 5, 5, 2, 5, 5, 9};
    const unsigned char *bytes = (const unsigned char *)db->mapping;
    struct knurl_mmdb_section section;
    struct knurl_mmdb_value map, value;
    size_t at, end, lower, tree;
    unsigned long budget = 65536UL, number;
    unsigned int i;
    int found;
    if (db->size < sizeof(marker))
        return 0;
    lower = db->size > 131072 ? db->size - 131072 : 0;
    at = db->size - sizeof(marker);
    for (;;) {
        if (memcmp(bytes + at, marker, sizeof(marker)) == 0)
            break;
        if (at == lower)
            return 0;
        --at;
    }
    end = at;
    section.data = bytes + at + sizeof(marker);
    section.size = db->size - at - sizeof(marker);
    if (!knurl_mmdb_decode(&section, 0, &map, &budget, 1) ||
        !unique_map(&section, &map, 0, &budget))
        return 0;
    for (i = 0; i < 7; ++i) {
        if (knurl_mmdb_select(&section, &map, names[i], &value, &budget) != 1 ||
            value.type != types[i])
            return 0;
        number = i == 3 || i == 6 ? 0 : knurl_mmdb_integer(&section, &value);
        if (i == 0)
            db->nodes = number;
        else if (i == 1)
            db->width = (unsigned int)number;
        else if (i == 2)
            db->ip_version = (unsigned int)number;
        else if (i == 4 && number != 2)
            return 0;
    }
    found = knurl_mmdb_select(&section, &map, "description", &value, &budget);
    if (found < 0 || (found && !unique_map(&section, &value, 1, &budget)))
        return 0;
    found = knurl_mmdb_select(&section, &map, "languages", &value, &budget);
    if (found < 0 || (found && value.type != 11))
        return 0;
    if (found) {
        at = value.payload;
        number = value.size;
        while (number-- != 0) {
            if (!knurl_mmdb_decode(&section, at, &value, &budget, 1) ||
                value.type != 2)
                return 0;
            at = value.next;
        }
    }
    if ((db->width != 24 && db->width != 28 && db->width != 32) ||
        (db->ip_version != 4 && db->ip_version != 6) || db->nodes == 0 ||
        db->nodes > ((size_t)-1) / (db->width / 4))
        return 0;
    tree = (size_t)db->nodes * (db->width / 4);
    if (tree > end || end - tree < 16)
        return 0;
    for (i = 0; i < 16; ++i)
        if (bytes[tree + i] != 0)
            return 0;
    db->data.data = bytes + tree + 16;
    db->data.size = end - tree - 16;
    if (db->data.size > 0xffffffffUL)
        return 0;
    db->ipv4 = 0;
    if (db->ip_version == 6)
        for (i = 0; i < 96 && db->ipv4 < db->nodes; ++i)
            db->ipv4 = branch(db, db->ipv4, 0);
    return 1;
}
static knurl_status lookup(const knurl_mmdb_address *address,
                           knurl_mmdb_country *country, void *userdata,
                           knurl_error *error)
{
    struct native_db *db = (struct native_db *)userdata;
    struct knurl_mmdb_value root, selected;
    unsigned long node, offset, budget = 65536UL;
    unsigned int bit;
    int found;
    if (address->size == 16 && db->ip_version == 4)
        return KNURL_EOF;
    node = address->size == 4 ? db->ipv4 : 0;
    for (bit = 0; bit < address->size * 8 && node < db->nodes; ++bit)
        node = branch(db, node, (address->bytes[bit / 8] >> (7 - bit % 8)) & 1);
    if (node == db->nodes)
        return KNURL_EOF;
    if (node < db->nodes || node - db->nodes < 16)
        return corrupt(error);
    offset = node - db->nodes - 16;
    if (!knurl_mmdb_decode(&db->data, (size_t)offset, &root, &budget, 1))
        return corrupt(error);
    found = knurl_mmdb_select(&db->data, &root, "country", &selected, &budget);
    if (found == 0)
        return KNURL_EOF;
    if (found < 0)
        return corrupt(error);
    found = knurl_mmdb_select(&db->data, &selected, "iso_code", &root, &budget);
    if (found == 0)
        return KNURL_EOF;
    if (found < 0 || root.type != 2 || root.size != 2)
        return corrupt(error);
    memcpy(country->code, db->data.data + root.payload, 2);
    country->code[2] = 0;
    return KNURL_OK;
}
knurl_status knurl_mmdb_native_open(const char *path, knurl_mmdb **out,
                                    knurl_error *error)
{
    struct stat info;
    struct native_db *db;
    knurl_mmdb_config config;
    knurl_status status;
    void *mapping;
    int fd, flags = O_RDONLY | O_NONBLOCK;
    if (out != NULL)
        *out = NULL;
    if (path == NULL || path[0] == 0 || out == NULL)
        return knurl_fail(error, KNURL_INVALID, "mmdb.open",
                          "path and output required");
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    fd = open(path, flags);
    if (fd < 0)
        return knurl_fail(error, KNURL_IO, "mmdb.open",
                          "cannot open database file");
#ifndef O_CLOEXEC
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
        close(fd);
        return knurl_fail(error, KNURL_IO, "mmdb.open",
                          "cannot protect file descriptor");
    }
#endif
    if (fstat(fd, &info) != 0) {
        close(fd);
        return knurl_fail(error, KNURL_IO, "mmdb.open",
                          "cannot stat database file");
    }
    if (!S_ISREG(info.st_mode) || info.st_size <= 0 ||
        (off_t)(size_t)info.st_size != info.st_size) {
        close(fd);
        return corrupt(error);
    }
    mapping = mmap(NULL, (size_t)info.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (mapping == MAP_FAILED)
        return knurl_fail(error, KNURL_IO, "mmdb.open",
                          "cannot map database file");
    db = (struct native_db *)calloc(1, sizeof(*db));
    if (db == NULL) {
        munmap(mapping, (size_t)info.st_size);
        return knurl_fail(error, KNURL_NOMEM, "mmdb.open", "allocation failed");
    }
    db->mapping = mapping;
    db->size = (size_t)info.st_size;
    if (!metadata(db)) {
        close_db(db);
        return corrupt(error);
    }
    knurl_mmdb_config_init(&config);
    config.lookup = lookup;
    config.close = close_db;
    config.userdata = db;
    status = knurl_mmdb_new(&config, out, error);
    if (status != KNURL_OK)
        close_db(db);
    return status;
}
