#include "internal.h"

#include <string.h>

#define WORD_MAX 0xffffffffUL
#define HALF_BASE 65536UL

int rotmos_u64_valid(rotmos_u64 value)
{
    return value.high <= WORD_MAX && value.low <= WORD_MAX;
}

rotmos_status rotmos_u64_from_words(unsigned long high, unsigned long low,
                                    rotmos_u64 *out, rotmos_error *error)
{
    if (out == NULL || high > WORD_MAX || low > WORD_MAX)
        return rotmos_fail(error, ROTMOS_INVALID, "u64_from_words",
                           "provide an output and two unsigned 32-bit words");
    out->high = high;
    out->low = low;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_status rotmos_u64_from_size(size_t value, rotmos_u64 *out,
                                   rotmos_error *error)
{
    rotmos_u64 result;
    if (out == NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "u64_from_size",
                           "output is required");
    result.low = (unsigned long)(value % HALF_BASE);
    value /= HALF_BASE;
    result.low += (unsigned long)(value % HALF_BASE) * HALF_BASE;
    value /= HALF_BASE;
    result.high = (unsigned long)(value % HALF_BASE);
    value /= HALF_BASE;
    result.high += (unsigned long)(value % HALF_BASE) * HALF_BASE;
    value /= HALF_BASE;
    if (value != 0)
        return rotmos_fail(error, ROTMOS_OVERFLOW, "u64_from_size",
                           "size exceeds the unsigned 64-bit range");
    *out = result;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_status rotmos_u64_to_size(rotmos_u64 value, size_t *out,
                                 rotmos_error *error)
{
    unsigned long parts[4];
    size_t result;
    size_t maximum;
    size_t i;
    if (out == NULL || !rotmos_u64_valid(value))
        return rotmos_fail(error, ROTMOS_INVALID, "u64_to_size",
                           "provide a valid value and output");
    parts[0] = value.high / HALF_BASE;
    parts[1] = value.high % HALF_BASE;
    parts[2] = value.low / HALF_BASE;
    parts[3] = value.low % HALF_BASE;
    result = 0;
    maximum = (size_t)-1;
    for (i = 0; i < 4; ++i) {
        if (result > (maximum - (size_t)parts[i]) / HALF_BASE)
            return rotmos_fail(error, ROTMOS_OVERFLOW, "u64_to_size",
                               "value cannot be represented by size_t");
        result = result * HALF_BASE + (size_t)parts[i];
    }
    *out = result;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_status rotmos_u64_compare(rotmos_u64 left, rotmos_u64 right, int *order,
                                 rotmos_error *error)
{
    if (order == NULL || !rotmos_u64_valid(left) || !rotmos_u64_valid(right))
        return rotmos_fail(error, ROTMOS_INVALID, "u64_compare",
                           "provide valid values and an order output");
    if (left.high != right.high)
        *order = left.high < right.high ? -1 : 1;
    else if (left.low != right.low)
        *order = left.low < right.low ? -1 : 1;
    else
        *order = 0;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_status rotmos_u64_add(rotmos_u64 left, rotmos_u64 right, rotmos_u64 *out,
                             rotmos_error *error)
{
    rotmos_u64 result;
    unsigned long carry;
    if (out == NULL || !rotmos_u64_valid(left) || !rotmos_u64_valid(right))
        return rotmos_fail(error, ROTMOS_INVALID, "u64_add",
                           "provide valid values and an output");
    result.low = (left.low + right.low) & WORD_MAX;
    carry = result.low < left.low ? 1UL : 0UL;
    result.high = (left.high + right.high) & WORD_MAX;
    if (result.high < left.high || (carry && result.high == WORD_MAX))
        return rotmos_fail(error, ROTMOS_OVERFLOW, "u64_add",
                           "unsigned 64-bit addition overflow");
    result.high += carry;
    *out = result;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_status rotmos_u64_from_decimal(const char *text, size_t length,
                                      rotmos_u64 *out, rotmos_error *error)
{
    rotmos_u64 result;
    rotmos_u64 twice;
    rotmos_u64 eight;
    rotmos_u64 digit;
    size_t i;
    rotmos_status status;
    if (text == NULL || length == 0 || out == NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "u64_from_decimal",
                           "provide decimal digits and an output");
    result.high = result.low = 0;
    digit.high = 0;
    for (i = 0; i < length; ++i) {
        if (text[i] < '0' || text[i] > '9')
            return rotmos_fail(error, ROTMOS_INVALID, "u64_from_decimal",
                               "unsigned decimal digits are required");
        digit.low = (unsigned long)(text[i] - '0');
        status = rotmos_u64_add(result, result, &twice, error);
        if (status == ROTMOS_OK)
            status = rotmos_u64_add(twice, twice, &eight, error);
        if (status == ROTMOS_OK)
            status = rotmos_u64_add(eight, eight, &eight, error);
        if (status == ROTMOS_OK)
            status = rotmos_u64_add(eight, twice, &result, error);
        if (status == ROTMOS_OK)
            status = rotmos_u64_add(result, digit, &result, error);
        if (status != ROTMOS_OK)
            return rotmos_fail(error, status, "u64_from_decimal",
                               "unsigned decimal value exceeds 64 bits");
    }
    *out = result;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_status rotmos_u64_format_decimal(rotmos_u64 value, char *buffer,
                                        size_t capacity, size_t *written,
                                        rotmos_error *error)
{
    unsigned long parts[4];
    unsigned long remainder;
    unsigned long combined;
    char digits[20];
    size_t count;
    size_t i;
    if (written != NULL)
        *written = 0;
    if (!rotmos_u64_valid(value) || buffer == NULL || written == NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "u64_format_decimal",
                           "provide a valid value, buffer and length output");
    parts[0] = value.high / HALF_BASE;
    parts[1] = value.high % HALF_BASE;
    parts[2] = value.low / HALF_BASE;
    parts[3] = value.low % HALF_BASE;
    count = 0;
    do {
        remainder = 0;
        for (i = 0; i < 4; ++i) {
            combined = remainder * HALF_BASE + parts[i];
            parts[i] = combined / 10UL;
            remainder = combined % 10UL;
        }
        digits[count++] = (char)('0' + remainder);
    } while (parts[0] || parts[1] || parts[2] || parts[3]);
    if (capacity <= count)
        return rotmos_fail(
            error, ROTMOS_LIMIT, "u64_format_decimal",
            "decimal buffer requires room for digits and a NUL terminator");
    for (i = 0; i < count; ++i)
        buffer[i] = digits[count - i - 1];
    buffer[count] = '\0';
    *written = count;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}
