#ifndef ROTMOS_ACCESS_LOG_FILE_H
#define ROTMOS_ACCESS_LOG_FILE_H
#include <rotmos/server.h>
struct rotmos_access_file;
rotmos_status rotmos_access_file_open(const rotmos_http_config *,
                                      struct rotmos_access_file **,
                                      rotmos_error *);
rotmos_status rotmos_access_file_write(struct rotmos_access_file *,
                                       const void *, size_t, rotmos_error *);
int rotmos_access_file_close(struct rotmos_access_file *);
#endif
