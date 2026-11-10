#include "internal.h"

#include <stdlib.h>
#include <string.h>

char *knurl_copy_text(const char *text)
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

int knurl_header_name_valid(const char *text)
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

int knurl_header_value_valid(const char *text)
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

static const unsigned char *mime_token(const unsigned char *p)
{
    while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
           (*p >= '0' && *p <= '9') ||
           (*p != 0 && strchr("!#$%&'*+-.^_`|~", *p) != NULL))
        ++p;
    return p;
}

/* RFC 9110 sections 5.6.2, 5.6.4, 5.6.6 and 8.3.1. Validate the complete
 * singleton value, not just its type/subtype prefix. Quoted obs-text is legal;
 * controls, DEL, field folding and comma-separated media-type lists are not. */
int knurl_content_type_valid(const char *text)
{
    const unsigned char *p = (const unsigned char *)text, *end;
    if (p == NULL)
        return 0;
    while (*p == ' ' || *p == '\t')
        ++p;
    end = mime_token(p);
    if (end == p || (*p == '*' && end == p + 1) || *end != '/')
        return 0;
    p = end + 1;
    end = mime_token(p);
    if (end == p || (*p == '*' && end == p + 1))
        return 0;
    p = end;
    for (;;) {
        while (*p == ' ' || *p == '\t')
            ++p;
        if (*p == 0)
            return 1;
        if (*p++ != ';')
            return 0;
        while (*p == ' ' || *p == '\t')
            ++p;
        /* The parameters ABNF explicitly permits an empty parameter. */
        if (*p == 0 || *p == ';')
            continue;
        end = mime_token(p);
        if (end == p || *end != '=')
            return 0;
        p = end + 1;
        if (*p == '"') {
            ++p;
            while (*p != '"') {
                if (*p == '\\')
                    ++p;
                if (*p != '\t' && (*p < 32 || *p == 127))
                    return 0;
                ++p;
            }
            ++p;
        } else {
            end = mime_token(p);
            if (end == p)
                return 0;
            p = end;
        }
    }
}

int knurl_text_equal_case(const char *left, const char *right)
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
