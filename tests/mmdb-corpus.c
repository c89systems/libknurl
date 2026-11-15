#include <knurl/mmdb.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned long next(unsigned long *state)
{
    *state = (*state * 1664525UL + 1013904223UL) & 0xffffffffUL;
    return *state;
}
static int expected(const knurl_mmdb_address *address)
{
    static const unsigned char mapped[12] = {0, 0, 0, 0, 0,   0,
                                             0, 0, 0, 0, 255, 255};
    static const unsigned char zero[12] = {0};
    const unsigned char *p = address->bytes;
    if (address->size == 16) {
        if (memcmp(p, zero, 12) == 0 || memcmp(p, mapped, 12) == 0)
            p += 12;
        else
            return p[0] == 0x20 && p[1] == 1 && p[2] == 0x0d && p[3] == 0xb8;
    }
    return p[0] >= 128 || (p[0] == 1 && p[1] == 2 && p[2] == 3);
}
int main(int argc, char **argv)
{
    knurl_mmdb *db;
    knurl_mmdb_address address;
    knurl_mmdb_country country;
    knurl_error error;
    knurl_status status;
    unsigned long i, j, state = 17, count = 65536, known = 0;
    unsigned char record[3];
    FILE *oracle = NULL;
    assert(argc == 2 || argc == 3 || argc == 4);
    if (argc >= 3)
        count = strtoul(argv[2], NULL, 10);
    if (argc == 4) {
        oracle = fopen(argv[3], "rb");
        assert(oracle != NULL);
    }
    assert(count > 0);
    assert(knurl_mmdb_open(argv[1], NULL, &db, &error) == KNURL_OK);
    for (i = 0; i < count; ++i) {
        memset(&address, 0, sizeof(address));
        address.size = i % 2 ? 4 : 16;
        for (j = 0; j < address.size; ++j)
            address.bytes[j] = (unsigned char)(next(&state) >> 24);
        if (address.size == 16 && i % 4 == 0)
            address.bytes[0] = (address.bytes[0] & 31) | 32;
        if (i % 32 == 0)
            assert(knurl_mmdb_address_parse("::ffff:1.2.3.4", &address,
                                            &error) == KNURL_OK);
        if (i % 32 == 1)
            assert(knurl_mmdb_address_parse("2001:db8::1", &address, &error) ==
                   KNURL_OK);
        status = db->lookup(db, &address, &country, &error);
        if (oracle != NULL) {
            assert(fread(record, 1, sizeof(record), oracle) == sizeof(record));
        } else {
            record[0] =
                (unsigned char)(expected(&address) ? KNURL_OK : KNURL_EOF);
            record[1] = record[0] == KNURL_OK ? 'U' : 0;
            record[2] = record[0] == KNURL_OK ? 'S' : 0;
        }
        if ((unsigned int)status != record[0] ||
            (unsigned char)country.code[0] != record[1] ||
            (unsigned char)country.code[1] != record[2]) {
            fprintf(stderr, "difference at %lu: %d %s (%s)\n", i, status,
                    country.code, error.message);
            return 1;
        }
        if (status == KNURL_OK)
            ++known;
    }
    if (oracle != NULL) {
        assert(fgetc(oracle) == EOF && !ferror(oracle));
        assert(fclose(oracle) == 0);
    }
    printf("matched=%lu known=%lu unknown=%lu\n", count, known, count - known);
    db->close(db, NULL);
    return 0;
}
