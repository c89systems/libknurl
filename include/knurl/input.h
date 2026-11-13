#ifndef KNURL_INPUT_H
#define KNURL_INPUT_H

/** @file input.h Exact validation of borrowed byte spans. No helper repairs,
 * trims or normalizes input. Validation is specific to the stated context. */
#include <knurl/common.h>
#ifdef __cplusplus
extern "C" {
#endif

/** Parse decimal C long within inclusive bounds; overflow is OVERFLOW. */
KNURL_API knurl_status knurl_input_long(knurl_bytes input, long minimum,
                                        long maximum, long *out,
                                        knurl_error *error);
/** Parse unsigned decimal with exact portable inclusive bounds. */
KNURL_API knurl_status knurl_input_u64(knurl_bytes input, knurl_u64 minimum,
                                       knurl_u64 maximum, knurl_u64 *out,
                                       knurl_error *error);
/** Accept exactly true/false/1/0; result is zero or one. */
KNURL_API knurl_status knurl_input_boolean(knurl_bytes input, int *out,
                                           knurl_error *error);
/** Validate UTF-8 and inclusive byte bounds. Nonzero rejects C0/C1 controls.
 * NUL is always invalid. Maximum zero means only an empty span can fit. */
KNURL_API knurl_status knurl_input_text(knurl_bytes input, size_t minimum,
                                        size_t maximum, int reject_controls,
                                        knurl_error *error);
/** Require every input byte in allowed and inclusive byte-length bounds. */
KNURL_API knurl_status knurl_input_allowlist(knurl_bytes input,
                                             knurl_bytes allowed,
                                             size_t minimum, size_t maximum,
                                             knurl_error *error);
/** Require exact membership in choices; empty choices never match. */
KNURL_API knurl_status knurl_input_choice(knurl_bytes input,
                                          const knurl_bytes *choices,
                                          size_t count, knurl_error *error);
/** ASCII dot-atom mailbox and DNS-label domain; no deliverability check. */
KNURL_API knurl_status knurl_input_email(knurl_bytes input, knurl_error *error);
/** Absolute HTTP/HTTPS URL with host and valid port, no credentials, controls,
 * whitespace or NUL. Uses libcurl parsing without network or DNS activity. */
KNURL_API knurl_status knurl_input_http_url(knurl_bytes input,
                                            knurl_error *error);
/** Single ASCII filename component, 1..255 bytes: letters/digits/._-;
 * excludes dot and dot-dot. Does not establish filesystem confinement. */
KNURL_API knurl_status knurl_input_filename(knurl_bytes input,
                                            knurl_error *error);
/** Escape &<> and both quotes for HTML text/quoted attributes. No JS/CSS/URL
 * safety promise. Empty owned output required; max_bytes=0 selects 1 MiB.
 * Validate UTF-8, return independent bytes; failure leaves output unchanged. */
KNURL_API knurl_status knurl_input_html_escape(knurl_bytes input,
                                               size_t max_bytes,
                                               knurl_owned_bytes *out,
                                               knurl_error *error);
#ifdef __cplusplus
}
#endif
#endif
