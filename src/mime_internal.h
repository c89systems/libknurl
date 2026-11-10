#ifndef KNURL_MIME_INTERNAL_H
#define KNURL_MIME_INTERNAL_H
#include <knurl/static.h>
knurl_status knurl_mime_copy(const knurl_mime_type *, size_t,
                             const knurl_mime_type **, knurl_error *);
void knurl_mime_free(const knurl_mime_type *, size_t);
const char *knurl_mime_resolve(const knurl_server *,
                               const knurl_static_config *, const char *,
                               const char *);
#endif
