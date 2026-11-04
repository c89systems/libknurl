#include "internal.h"

#include <stdlib.h>
#include <string.h>

char *rotmos_copy_text(const char *text)
{
    char *copy;
    size_t length;
    if (text == NULL)
        return NULL;
    length = strlen(text);
    if (length == (size_t)-1)
        return NULL;
    copy = malloc(length + 1);
    if (copy != NULL)
        memcpy(copy, text, length + 1);
    return copy;
}

int rotmos_header_name_valid(const char *text)
{
    const unsigned char *byte;
    if (text == NULL || *text == '\0')
        return 0;
    for (byte = (const unsigned char *)text; *byte != 0; ++byte) {
        if (!((*byte >= 'a' && *byte <= 'z') ||
              (*byte >= 'A' && *byte <= 'Z') ||
              (*byte >= '0' && *byte <= '9') ||
              strchr("!#$%&'*+-.^_`|~", *byte) != NULL))
            return 0;
    }
    return 1;
}

int rotmos_header_value_valid(const char *text)
{
    const unsigned char *byte;
    if (text == NULL)
        return 0;
    for (byte = (const unsigned char *)text; *byte != 0; ++byte) {
        if ((*byte < 32 && *byte != '\t') || *byte == 127)
            return 0;
    }
    return 1;
}

int rotmos_text_equal_case(const char *left, const char *right)
{
    unsigned char a;
    unsigned char b;
    do {
        a = (unsigned char)*left++;
        b = (unsigned char)*right++;
        if (a >= 'A' && a <= 'Z')
            a = (unsigned char)(a + 'a' - 'A');
        if (b >= 'A' && b <= 'Z')
            b = (unsigned char)(b + 'a' - 'A');
        if (a != b)
            return 0;
    } while (a != 0);
    return 1;
}
