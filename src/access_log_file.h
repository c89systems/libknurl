#ifndef KNURL_ACCESS_LOG_FILE_H
#define KNURL_ACCESS_LOG_FILE_H
#include <knurl/server.h>
struct knurl_access_file;
knurl_status knurl_access_file_open(const knurl_http_config *,
                                    struct knurl_access_file **, knurl_error *);
knurl_status knurl_access_file_write(struct knurl_access_file *, const void *,
                                     size_t, knurl_error *);
int knurl_access_file_close(struct knurl_access_file *);
#endif
