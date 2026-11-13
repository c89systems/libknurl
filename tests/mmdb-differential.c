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
int main(int argc, char **argv)
{
    knurl_mmdb *reference, *native;
    knurl_mmdb_address address;
    knurl_mmdb_country a, b;
    knurl_error error;
    knurl_status sa, sb;
    unsigned long i, j, state = 17, count = 1000000, known = 0;
    assert(argc == 2 || argc == 3);
    if (argc == 3)
        count = strtoul(argv[2], NULL, 10);
    assert(count > 0);
    assert(knurl_mmdb_maxmind_open(argv[1], &reference, &error) == KNURL_OK);
    assert(knurl_mmdb_native_open(argv[1], &native, &error) == KNURL_OK);
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
        sa = reference->lookup(reference, &address, &a, &error);
        assert(sa == KNURL_OK || sa == KNURL_EOF);
        sb = native->lookup(native, &address, &b, &error);
        if (sa != sb || strcmp(a.code, b.code) != 0) {
            fprintf(stderr, "difference at %lu: %d %s versus %d %s (%s)\n", i,
                    sa, a.code, sb, b.code, error.message);
            return 1;
        }
        if (sa == KNURL_OK)
            ++known;
    }
    printf("matched=%lu known=%lu unknown=%lu\n", count, known, count - known);
    reference->close(reference, NULL);
    native->close(native, NULL);
    return 0;
}
