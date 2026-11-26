#ifndef KNURL_LUA_CURL_INTERNAL_H
#define KNURL_LUA_CURL_INTERNAL_H
#ifndef LONEJSON_WITH_CURL
#define LONEJSON_WITH_CURL 1
#endif
#include "binding.h"
#include <curl/curl.h>
#include <lonejson_lua.h>
#include <sys/stat.h>
#define RL_CURL_CALL "knurl.curl_call"
#define RL_CURL_BODY_LIMIT (8UL * 1024UL * 1024UL)
#define RL_CURL_HEADER_LIMIT 65536UL
struct rl_curl_mime_file {
    struct rl_curl_mime_file *next;
    FILE *stream;
    struct stat info;
};
struct rl_curl_call {
    lua_State *L;
    CURL *easy;
    struct curl_slist *headers;
    struct curl_slist *recipients;
    curl_mime *mime;
    struct rl_curl_mime_file *mime_files;
    size_t body_length, header_length, received;
    unsigned char *body, *head;
    size_t body_capacity, header_capacity;
    char error[CURL_ERROR_SIZE];
    char callback_message[256];
    int initialized, stopped, code, callback_error;
    long status;
    char *input_path, *output_path;
    int read_callback, write_callback, progress_callback, json_read, json_write,
        download;
    int records[2];
    lonejson_curl_parse parser;
    lonejson_curl_upload upload;
    FILE *input, *output;
    char *request_body;
    size_t request_length, request_offset;
};
int rl_curl_open_file(const char *path, int flags, struct stat *info);
int rl_curl_file_seek(void *data, curl_off_t offset, int origin);
knurl_status rl_curl_config(lua_State *, struct rl_curl_call *, knurl_error *);
knurl_status rl_curl_json_config(lua_State *, struct rl_curl_call *,
                                 knurl_error *);
void rl_curl_json_result(lua_State *, struct rl_curl_call *);
#endif
