#include "internal.h"
#include <knurl/input.h>
#include <curl/curl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static int span(knurl_bytes input)
{
    return (input.data != NULL || input.length == 0) &&
           (input.length == 0 || memchr(input.data, 0, input.length) == NULL);
}
static knurl_status invalid(knurl_error *error, const char *message)
{
    return knurl_fail(error, KNURL_INVALID, "input", message);
}
knurl_status knurl_input_long(knurl_bytes input, long minimum, long maximum,
                              long *out, knurl_error *error)
{
    const unsigned char *p = input.data;
    unsigned long value = 0, limit, digit;
    long result;
    size_t i = 0;
    int negative;
    if (!span(input) || input.length == 0 || out == NULL || minimum > maximum)
        return invalid(error,
                       "provide decimal digits, ordered bounds and output");
    negative = p[0] == '-';
    if (negative)
        ++i;
    if (i == input.length)
        return invalid(error, "minus must be followed by decimal digits");
    limit = negative ? (unsigned long)LONG_MAX + 1UL : (unsigned long)LONG_MAX;
    for (; i < input.length; ++i) {
        if (p[i] < '0' || p[i] > '9')
            return invalid(error,
                           "integer requires only optional minus and digits");
        digit = (unsigned long)(p[i] - '0');
        if (value > (limit - digit) / 10UL)
            return knurl_fail(error, KNURL_OVERFLOW, "input_long",
                              "decimal value exceeds C long range");
        value = value * 10UL + digit;
    }
    result = negative ? (value == (unsigned long)LONG_MAX + 1UL ? LONG_MIN
                                                                : -(long)value)
                      : (long)value;
    if (result < minimum || result > maximum)
        return invalid(error, "integer is outside the declared bounds");
    *out = result;
    knurl_error_clear(error);
    return KNURL_OK;
}
knurl_status knurl_input_u64(knurl_bytes input, knurl_u64 minimum,
                             knurl_u64 maximum, knurl_u64 *out,
                             knurl_error *error)
{
    knurl_u64 value;
    knurl_status status;
    int order;
    if (!span(input) || out == NULL ||
        knurl_u64_compare(minimum, maximum, &order, NULL) != KNURL_OK ||
        order > 0)
        return invalid(error,
                       "provide unsigned decimal, ordered bounds and output");
    status = knurl_u64_from_decimal(input.data, input.length, &value, error);
    if (status != KNURL_OK)
        return status;
    (void)knurl_u64_compare(value, minimum, &order, NULL);
    if (order < 0)
        return invalid(error, "unsigned integer is below the declared minimum");
    (void)knurl_u64_compare(value, maximum, &order, NULL);
    if (order > 0)
        return invalid(error, "unsigned integer exceeds the declared maximum");
    *out = value;
    knurl_error_clear(error);
    return KNURL_OK;
}
knurl_status knurl_input_boolean(knurl_bytes input, int *out,
                                 knurl_error *error)
{
    int result;
    if (!span(input) || out == NULL)
        return invalid(error, "provide a boolean span and output");
    if ((input.length == 4 && memcmp(input.data, "true", 4) == 0) ||
        (input.length == 1 && memcmp(input.data, "1", 1) == 0))
        result = 1;
    else if ((input.length == 5 && memcmp(input.data, "false", 5) == 0) ||
             (input.length == 1 && memcmp(input.data, "0", 1) == 0))
        result = 0;
    else
        return invalid(error, "boolean must be exactly true, false, 1 or 0");
    *out = result;
    knurl_error_clear(error);
    return KNURL_OK;
}
knurl_status knurl_input_text(knurl_bytes input, size_t minimum, size_t maximum,
                              int reject_controls, knurl_error *error)
{
    const unsigned char *p = input.data;
    unsigned long value, smallest;
    size_t i = 0;
    unsigned int need;
    if (!span(input) || minimum > maximum || input.length < minimum ||
        input.length > maximum)
        return invalid(error,
                       "text must fit the declared byte bounds without NUL");
    while (i < input.length) {
        value = p[i++];
        need = 0;
        smallest = 0;
        if (value >= 194 && value <= 223) {
            need = 1;
            smallest = 128;
            value &= 31;
        } else if (value >= 224 && value <= 239) {
            need = 2;
            smallest = 2048;
            value &= 15;
        } else if (value >= 240 && value <= 244) {
            need = 3;
            smallest = 65536UL;
            value &= 7;
        } else if (value >= 128)
            return invalid(error, "text is not valid UTF-8");
        if (input.length - i < need)
            return invalid(error, "text ends within a UTF-8 sequence");
        while (need-- != 0) {
            if ((p[i] & 192) != 128)
                return invalid(error, "text is not valid UTF-8");
            value = (value << 6) | (p[i++] & 63);
        }
        if (value < smallest || value > 0x10ffffUL ||
            (value >= 0xd800UL && value <= 0xdfffUL))
            return invalid(error, "text contains an invalid Unicode scalar");
        if (reject_controls && (value < 32 || (value >= 127 && value <= 159)))
            return invalid(error, "text contains a control character");
    }
    knurl_error_clear(error);
    return KNURL_OK;
}
knurl_status knurl_input_allowlist(knurl_bytes input, knurl_bytes allowed,
                                   size_t minimum, size_t maximum,
                                   knurl_error *error)
{
    const unsigned char *p = input.data;
    size_t i;
    if (!span(input) || !span(allowed) || minimum > maximum ||
        input.length < minimum || input.length > maximum)
        return invalid(error, "provide valid spans and ordered byte bounds");
    for (i = 0; i < input.length; ++i)
        if (allowed.length == 0 ||
            memchr(allowed.data, p[i], allowed.length) == NULL)
            return invalid(error,
                           "input contains a byte outside the allowlist");
    knurl_error_clear(error);
    return KNURL_OK;
}
knurl_status knurl_input_choice(knurl_bytes input, const knurl_bytes *choices,
                                size_t count, knurl_error *error)
{
    size_t i;
    if (!span(input) || (choices == NULL && count != 0))
        return invalid(error, "provide input and a choice array");
    for (i = 0; i < count; ++i)
        if (span(choices[i]) && input.length == choices[i].length &&
            (input.length == 0 ||
             memcmp(input.data, choices[i].data, input.length) == 0)) {
            knurl_error_clear(error);
            return KNURL_OK;
        }
    return invalid(error, "input is not one of the declared choices");
}
static int alnum_ascii(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}
knurl_status knurl_input_email(knurl_bytes input, knurl_error *error)
{
    const unsigned char *p = input.data;
    size_t i, at = 0, label;
    if (!span(input) || input.length < 3 || input.length > 254)
        return invalid(error,
                       "email must be an ASCII mailbox of at most 254 bytes");
    while (at < input.length && p[at] != '@')
        ++at;
    if (at == 0 || at > 64 || at == input.length || p[0] == '.' ||
        p[at - 1] == '.' || input.length - at - 1 > 253)
        return invalid(error,
                       "email requires a dot-atom local part and domain");
    for (i = 0; i < at; ++i)
        if ((!alnum_ascii(p[i]) &&
             strchr("!#$%&'*+-/=?^_`{|}~.", p[i]) == NULL) ||
            (p[i] == '.' && i != 0 && p[i - 1] == '.'))
            return invalid(
                error, "email local part is outside the ASCII dot-atom subset");
    label = at + 1;
    for (i = label; i <= input.length; ++i) {
        if (i == input.length || p[i] == '.') {
            if (i == label || i - label > 63 || p[label] == '-' ||
                p[i - 1] == '-')
                return invalid(error, "email domain has an invalid DNS label");
            label = i + 1;
        } else if (!alnum_ascii(p[i]) && p[i] != '-')
            return invalid(error, "email domain requires ASCII DNS labels");
    }
    knurl_error_clear(error);
    return KNURL_OK;
}
knurl_status knurl_input_http_url(knurl_bytes input, knurl_error *error)
{
    const unsigned char *p = input.data;
    CURLU *url;
    CURLUcode code;
    char *text, *scheme = NULL, *host = NULL;
    size_t i;
    int valid;
    if (!span(input) || input.length == 0 || input.length == (size_t)-1)
        return invalid(error, "provide an absolute HTTP or HTTPS URL");
    for (i = 0; i < input.length; ++i)
        if (p[i] <= 32 || p[i] == 127)
            return invalid(error, "URL contains whitespace or control bytes");
    text = malloc(input.length + 1);
    if (text == NULL)
        return knurl_fail(error, KNURL_NOMEM, "input_http_url",
                          "cannot copy URL");
    memcpy(text, input.data, input.length);
    text[input.length] = 0;
    url = curl_url();
    if (url == NULL) {
        free(text);
        return knurl_fail(error, KNURL_NOMEM, "input_http_url",
                          "cannot allocate URL parser");
    }
    code = curl_url_set(url, CURLUPART_URL, text, CURLU_DISALLOW_USER);
    if (code == CURLUE_OK)
        code = curl_url_get(url, CURLUPART_SCHEME, &scheme, 0);
    if (code == CURLUE_OK)
        code = curl_url_get(url, CURLUPART_HOST, &host, 0);
    valid = code == CURLUE_OK && host[0] != 0 &&
            (knurl_text_equal_case(scheme, "http") ||
             knurl_text_equal_case(scheme, "https"));
    /* libcurl otherwise guesses a scheme for a bare hostname. */
    if (valid) {
        i = strlen(scheme);
        valid = input.length > i + 3 && memcmp(text + i, "://", 3) == 0 &&
                text[i + 3] != '/';
    }
    curl_free(scheme);
    curl_free(host);
    curl_url_cleanup(url);
    free(text);
    if (code == CURLUE_OUT_OF_MEMORY)
        return knurl_fail(error, KNURL_NOMEM, "input_http_url",
                          "URL parser allocation failed");
    if (!valid)
        return invalid(error, "URL needs an explicit HTTP(S) scheme, host and "
                              "valid port without credentials");
    knurl_error_clear(error);
    return KNURL_OK;
}
knurl_status knurl_input_filename(knurl_bytes input, knurl_error *error)
{
    const unsigned char *p = input.data;
    size_t i;
    if (!span(input) || input.length == 0 || input.length > 255 ||
        (input.length == 1 && p[0] == '.') ||
        (input.length == 2 && p[0] == '.' && p[1] == '.'))
        return invalid(
            error, "filename must be one non-dot component of 1..255 bytes");
    for (i = 0; i < input.length; ++i)
        if (!alnum_ascii(p[i]) && p[i] != '.' && p[i] != '_' && p[i] != '-')
            return invalid(error, "filename permits only ASCII letters, "
                                  "digits, dot, underscore and hyphen");
    knurl_error_clear(error);
    return KNURL_OK;
}
knurl_status knurl_input_html_escape(knurl_bytes input, size_t max_bytes,
                                     knurl_owned_bytes *out, knurl_error *error)
{
    const unsigned char *p = input.data;
    const char *replacement;
    char *bytes;
    size_t i, length = 0, count, at = 0;
    knurl_status status;
    if (out == NULL || out->data != NULL || out->length != 0)
        return invalid(error, "provide an initialized empty owned byte output");
    status = knurl_input_text(input, 0, (size_t)-1, 0, error);
    if (status != KNURL_OK)
        return status;
    if (max_bytes == 0)
        max_bytes = 1048576UL;
    for (i = 0; i < input.length; ++i) {
        count = p[i] == '&'                  ? 5
                : p[i] == '<' || p[i] == '>' ? 4
                : p[i] == '"'                ? 6
                : p[i] == '\''               ? 5
                                             : 1;
        if (count > max_bytes - length)
            return knurl_fail(error, KNURL_LIMIT, "input_html_escape",
                              "escaped HTML exceeds output bound");
        length += count;
    }
    if (length == (size_t)-1)
        return knurl_fail(error, KNURL_OVERFLOW, "input_html_escape",
                          "escaped HTML size overflows");
    bytes = malloc(length + 1);
    if (bytes == NULL)
        return knurl_fail(error, KNURL_NOMEM, "input_html_escape",
                          "cannot allocate escaped HTML");
    for (i = 0; i < input.length; ++i) {
        replacement = p[i] == '&'    ? "&amp;"
                      : p[i] == '<'  ? "&lt;"
                      : p[i] == '>'  ? "&gt;"
                      : p[i] == '"'  ? "&quot;"
                      : p[i] == '\'' ? "&#39;"
                                     : NULL;
        if (replacement != NULL) {
            count = strlen(replacement);
            memcpy(bytes + at, replacement, count);
            at += count;
        } else
            bytes[at++] = (char)p[i];
    }
    bytes[length] = 0;
    out->data = bytes;
    out->length = length;
    knurl_error_clear(error);
    return KNURL_OK;
}
