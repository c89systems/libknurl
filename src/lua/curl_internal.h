#ifndef KNURL_LUA_CURL_INTERNAL_H
#define KNURL_LUA_CURL_INTERNAL_H
#ifndef LONEJSON_WITH_CURL
#define LONEJSON_WITH_CURL 1
#endif
#include "binding.h"
#include <curl/curl.h>
#include <lonejson_lua.h>
#define RL_CURL_CALL "knurl.curl_call"
#define RL_CURL_BODY_LIMIT (8UL * 1024UL * 1024UL)
#define RL_CURL_HEADER_LIMIT 65536UL
struct rl_curl_call {
    lua_State *L;
    knurl_task *task;
    CURL *easy;
    struct curl_slist *headers;
    struct curl_slist *recipients;
    curl_mime *mime;
    size_t body_length, header_length, received;
    unsigned char *body, *head;
    size_t body_capacity, header_capacity;
    char error[CURL_ERROR_SIZE];
    char callback_message[256];
    int options, stopped, code, owned, callback_error;
    long status;
    long timeout_ms;
    char *effective_url;
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
int rl_curl_core(lua_State *);
int rl_curl_loader(lua_State *);
int rl_async_curl_core(lua_State *);
int rl_async_curl_loader(lua_State *);
extern char rl_curl_core_key, rl_curl_facade_key;
extern char rl_async_curl_core_key, rl_async_curl_facade_key;
void rl_curl_stop(struct rl_curl_call *);
knurl_status rl_curl_config(lua_State *, struct rl_curl_call *, knurl_error *);
knurl_status rl_curl_json_config(lua_State *, struct rl_curl_call *,
                                 knurl_error *);
void rl_curl_json_result(lua_State *, struct rl_curl_call *);
#endif
