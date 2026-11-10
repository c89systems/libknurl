#include "kore.h"
#include "sha2.h"

/* The isolated hash object has one dependency on Kore's zeroing primitive. */
void kore_mem_zero(void *memory, size_t size)
{
    volatile unsigned char *bytes;
    bytes = memory;
    while (size-- != 0)
        *bytes++ = 0;
}

static int digest_matches(const unsigned char *digest, size_t length,
                          const char *expected)
{
    static const char digits[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < length; ++i) {
        if (expected[2 * i] != digits[digest[i] >> 4] ||
            expected[2 * i + 1] != digits[digest[i] & 15])
            return 0;
    }
    return 1;
}

int main(void)
{
    SHA2_CTX context;
    unsigned char digest[SHA512_DIGEST_LENGTH];
    SHA256Init(&context);
    SHA256Update(&context, (const unsigned char *)"abc", 3);
    SHA256Final(digest, &context);
    if (!digest_matches(digest, SHA256_DIGEST_LENGTH,
                        "ba7816bf8f01cfea414140de5dae2223b0"
                        "0361a396177a9cb410ff61f20015ad"))
        return 1;
    SHA384Init(&context);
    SHA384Update(&context, (const unsigned char *)"abc", 3);
    SHA384Final(digest, &context);
    if (!digest_matches(digest, SHA384_DIGEST_LENGTH,
                        "cb00753f45a35e8bb5a03d699ac65007"
                        "272c32ab0eded1631a8b605a43ff5bed"
                        "8086072ba1e7cc2358baeca134c825a7"))
        return 2;
    SHA512Init(&context);
    SHA512Update(&context, (const unsigned char *)"abc", 3);
    SHA512Final(digest, &context);
    if (!digest_matches(digest, SHA512_DIGEST_LENGTH,
                        "ddaf35a193617abacc417349ae204131"
                        "12e6fa4e89a97ea20a9eeee64b55d39a"
                        "2192992a274fc1a836ba3c23a3feebbd"
                        "454d4423643ce80e2a9ac94fa54ca49f"))
        return 3;
    return 0;
}
