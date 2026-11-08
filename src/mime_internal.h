#ifndef ROTMOS_MIME_INTERNAL_H
#define ROTMOS_MIME_INTERNAL_H
#include <rotmos/static.h>
rotmos_status rotmos_mime_copy(const rotmos_mime_type *, size_t,
                               const rotmos_mime_type **, rotmos_error *);
void rotmos_mime_free(const rotmos_mime_type *, size_t);
const char *rotmos_mime_resolve(const rotmos_server *,
                                const rotmos_static_config *, const char *,
                                const char *);
#endif
