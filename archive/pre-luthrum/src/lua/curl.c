/* Handwritten native async adapter. Copyright (c) 2026 C89 Systems AB.
 * MIT License; synchronous facade adapted from Vectis. */
#include "curl_internal.h"
#include "curl_facade.inc"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>

char rl_curl_core_key, rl_curl_facade_key;
char rl_async_curl_core_key, rl_async_curl_facade_key;
static char callback_key;
static pthread_once_t curl_once = PTHREAD_ONCE_INIT;
static CURLcode curl_initialized;
static void initialize_curl(void)
{
    curl_initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
}
static int in_callback(lua_State *L)
{
    int value;
    lua_rawgetp(L, LUA_REGISTRYINDEX, &callback_key);
    value = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return value;
}
void rl_curl_stop(struct rl_curl_call *c)
{
    if (c->stopped)
        return;
    c->stopped = 1;
    curl_slist_free_all(c->headers);
    c->headers = NULL;
    curl_slist_free_all(c->recipients);
    c->recipients = NULL;
    if (c->mime != NULL) {
        curl_mime_free(c->mime);
        c->mime = NULL;
    }
    if (c->easy != NULL && c->owned)
        curl_easy_cleanup(c->easy);
    c->easy = NULL;
    if (c->input != NULL) {
        fclose(c->input);
        c->input = NULL;
    }
    if (c->output != NULL) {
        fclose(c->output);
        c->output = NULL;
    }
}
static int gc(lua_State *L)
{
    struct rl_curl_call *c = luaL_checkudata(L, 1, RL_CURL_CALL);
    rl_curl_stop(c);
    if (c->json_read) {
        lonejson_curl_upload_cleanup(&c->upload);
        c->json_read = 0;
    }
    if (c->json_write) {
        lonejson_curl_parse_cleanup(&c->parser);
        c->json_write = 0;
    }
    luaL_unref(L, LUA_REGISTRYINDEX, c->records[0]);
    c->records[0] = LUA_NOREF;
    luaL_unref(L, LUA_REGISTRYINDEX, c->records[1]);
    c->records[1] = LUA_NOREF;
    luaL_unref(L, LUA_REGISTRYINDEX, c->options);
    c->options = LUA_NOREF;
    free(c->body);
    c->body = NULL;
    free(c->head);
    c->head = NULL;
    free(c->request_body);
    c->request_body = NULL;
    free(c->effective_url);
    c->effective_url = NULL;
    free(c->input_path);
    c->input_path = NULL;
    free(c->output_path);
    c->output_path = NULL;
    return 0;
}
static int append(unsigned char **buffer, size_t *used, size_t *allocated,
                  const void *bytes, size_t count, size_t limit)
{
    size_t capacity;
    unsigned char *next;
    if (count > limit - *used)
        return 0;
    if (*used + count > *allocated) {
        capacity = *allocated ? *allocated : 1024;
        while (capacity < *used + count) {
            if (capacity > limit / 2) {
                capacity = limit;
                break;
            }
            capacity *= 2;
        }
        next = realloc(*buffer, capacity);
        if (next == NULL)
            return 0;
        *buffer = next;
        *allocated = capacity;
    }
    if (count)
        memcpy(*buffer + *used, bytes, count);
    *used += count;
    return 1;
}
/* The task thread waits while this small descriptor and libcurl's bounded
 * chunk are borrowed by the owner callback. No Lua is touched by that thread.
 */
struct callback_call {
    struct rl_curl_call *call;
    int kind, accepted;
    void *bytes;
    size_t length, produced;
    curl_off_t progress[4];
};
static int callback_trampoline(lua_State *L)
{
    static const char *const names[] = {"write", "read", "progress"};
    static const char *const fields[] = {"download_total", "downloaded",
                                         "upload_total", "uploaded"};
    struct callback_call *v = lua_touserdata(L, 1);
    struct rl_curl_call *c = v->call;
    const char *bytes;
    size_t length, i;
    if (v->kind == 3) {
        v->produced =
            lonejson_curl_write_callback(v->bytes, 1, v->length, &c->parser);
        v->accepted = v->produced == v->length;
        return 0;
    }
    if (v->kind == 4) {
        v->produced =
            lonejson_curl_read_callback(v->bytes, 1, v->length, &c->upload);
        v->accepted = v->produced != CURL_READFUNC_ABORT;
        return 0;
    }
    if (v->kind == 5) {
        v->produced = (size_t)lonejson_curl_seek_callback(
            &c->upload, v->progress[0], (int)v->progress[1]);
        v->accepted = 1;
        return 0;
    }
    lua_pushboolean(L, 1);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &callback_key);
    lua_rawgeti(L, LUA_REGISTRYINDEX, c->options);
    lua_getfield(L, -1, names[v->kind]);
    if (v->kind == 0)
        lua_pushlstring(L, v->bytes, v->length);
    else if (v->kind == 1)
        lua_pushinteger(L, (lua_Integer)v->length);
    else {
        lua_newtable(L);
        for (i = 0; i < 4; ++i) {
            lua_pushinteger(L, (lua_Integer)v->progress[i]);
            lua_setfield(L, -2, fields[i]);
        }
    }
    lua_call(L, 1, 2);
    if (!lua_isnil(L, -1))
        return luaL_error(L, "curl callback returned an error");
    if (v->kind != 1)
        v->accepted = lua_isboolean(L, -2) && lua_toboolean(L, -2);
    else if (lua_isnil(L, -2))
        v->accepted = 1;
    else if (lua_type(L, -2) == LUA_TSTRING) {
        bytes = lua_tolstring(L, -2, &length);
        if (length > 0 && length <= v->length) {
            memcpy(v->bytes, bytes, length);
            v->produced = length;
            v->accepted = 1;
        }
    }
    return 0;
}
static knurl_status callback_owner(void *data, knurl_error *error)
{
    struct callback_call *v = data;
    struct rl_curl_call *c = v->call;
    knurl_status status = rl_protect(c->L, callback_trampoline, v, error);
    lua_pushboolean(c->L, 0);
    lua_rawsetp(c->L, LUA_REGISTRYINDEX, &callback_key);
    return status;
}
static int callback_invoke(struct callback_call *v)
{
    knurl_error error;
    struct rl_curl_call *c = v->call;
    knurl_status status =
        c->task != NULL ? knurl_task_call(c->task, callback_owner, v, &error)
                        : callback_owner(v, &error);
    if (status != KNURL_OK || !v->accepted) {
        c->callback_error = 1;
        snprintf(
            c->callback_message, sizeof(c->callback_message), "%s",
            status != KNURL_OK
                ? error.message
                : "curl callback rejected data or returned an invalid result");
    }
    return status == KNURL_OK && v->accepted;
}
static size_t body_write(char *data, size_t size, size_t count, void *userdata)
{
    struct rl_curl_call *c = userdata;
    size_t length;
    struct callback_call callback;
    if (size != 0 && count > (size_t)-1 / size)
        return 0;
    length = size * count;
    if (c->output != NULL)
        return fwrite(data, 1, length, c->output);
    if (c->write_callback || c->json_write) {
        if (c->json_write) {
            if (length > RL_CURL_BODY_LIMIT - c->received)
                return 0;
            c->received += length;
        }
        memset(&callback, 0, sizeof(callback));
        callback.call = c;
        callback.bytes = data;
        callback.length = length;
        callback.kind = c->json_write ? 3 : 0;
        return callback_invoke(&callback) ? length : 0;
    }
    if (!append(&c->body, &c->body_length, &c->body_capacity, data, length,
                RL_CURL_BODY_LIMIT)) {
        snprintf(c->error, sizeof(c->error),
                 "curl buffered response exceeds 8 MiB or allocation failed");
        return 0;
    }
    return length;
}
static size_t head_write(char *data, size_t size, size_t count, void *userdata)
{
    struct rl_curl_call *c = userdata;
    if (size != 0 && count > (size_t)-1 / size)
        return 0;
    return append(&c->head, &c->header_length, &c->header_capacity, data,
                  size * count, RL_CURL_HEADER_LIMIT)
               ? size * count
               : 0;
}
static size_t body_read(char *data, size_t size, size_t count, void *userdata)
{
    struct rl_curl_call *c = userdata;
    size_t capacity;
    struct callback_call callback;
    if (size != 0 && count > (size_t)-1 / size)
        return CURL_READFUNC_ABORT;
    capacity = size * count;
    if (c->input != NULL) {
        capacity = fread(data, 1, capacity, c->input);
        return ferror(c->input) ? CURL_READFUNC_ABORT : capacity;
    }
    if (c->request_body != NULL) {
        if (capacity > c->request_length - c->request_offset)
            capacity = c->request_length - c->request_offset;
        memcpy(data, c->request_body + c->request_offset, capacity);
        c->request_offset += capacity;
        return capacity;
    }
    if (capacity > 65536)
        capacity = 65536;
    memset(&callback, 0, sizeof(callback));
    callback.call = c;
    callback.kind = c->json_read ? 4 : 1;
    callback.bytes = data;
    callback.length = capacity;
    return callback_invoke(&callback) ? callback.produced : CURL_READFUNC_ABORT;
}
static int seek(void *data, curl_off_t offset, int origin)
{
    struct callback_call callback;
    memset(&callback, 0, sizeof(callback));
    callback.call = data;
    callback.kind = 5;
    callback.progress[0] = offset;
    callback.progress[1] = origin;
    return callback_invoke(&callback) ? (int)callback.produced
                                      : CURL_SEEKFUNC_FAIL;
}
static int progress(void *data, curl_off_t dt, curl_off_t dn, curl_off_t ut,
                    curl_off_t un)
{
    struct rl_curl_call *c = data;
    struct callback_call callback;
    if (c->task != NULL && knurl_task_cancelled(c->task))
        return 1;
    if (!c->progress_callback)
        return 0;
    memset(&callback, 0, sizeof(callback));
    callback.call = c;
    callback.kind = 2;
    callback.progress[0] = dt;
    callback.progress[1] = dn;
    callback.progress[2] = ut;
    callback.progress[3] = un;
    return callback_invoke(&callback) ? 0 : 1;
}
static knurl_status execute(knurl_task *task, void *data, knurl_error *error)
{
    struct rl_curl_call *c = data;
    char *url = NULL;
    struct stat info;
    int fd;
    c->task = task;
    /* Open nonblocking first so a FIFO cannot occupy a task indefinitely;
     * only regular files are accepted, then normal file I/O is restored. */
    if (c->input_path != NULL) {
        fd = open(c->input_path, O_RDONLY | O_NONBLOCK);
        if (fd < 0 || fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) ||
            fcntl(fd, F_SETFL, 0) != 0 ||
            (c->input = fdopen(fd, "rb")) == NULL) {
            if (fd >= 0)
                close(fd);
            c->code = CURLE_READ_ERROR;
            snprintf(c->error, sizeof(c->error),
                     "curl upload_path must be a readable regular file");
            goto finished;
        }
        if (curl_easy_setopt(c->easy, CURLOPT_INFILESIZE_LARGE,
                             (curl_off_t)info.st_size) != CURLE_OK) {
            c->code = CURLE_READ_ERROR;
            goto finished;
        }
    }
    if (c->output_path != NULL) {
        fd = open(c->output_path, O_WRONLY | O_CREAT | O_NONBLOCK, 0666);
        if (fd < 0 || fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) ||
            fcntl(fd, F_SETFL, 0) != 0 || ftruncate(fd, 0) != 0 ||
            (c->output = fdopen(fd, "wb")) == NULL) {
            if (fd >= 0)
                close(fd);
            c->code = CURLE_WRITE_ERROR;
            snprintf(c->error, sizeof(c->error),
                     "curl download_path must be a writable regular file");
            goto finished;
        }
    }
    c->code = (int)curl_easy_perform(c->easy);
    if (c->output != NULL) {
        int failed = fflush(c->output) != 0;
        if (fclose(c->output) != 0)
            failed = 1;
        c->output = NULL;
        if (failed) {
            c->code = CURLE_WRITE_ERROR;
            snprintf(c->error, sizeof(c->error),
                     "closing curl download failed");
        }
    }
    (void)curl_easy_getinfo(c->easy, CURLINFO_RESPONSE_CODE, &c->status);
    (void)curl_easy_getinfo(c->easy, CURLINFO_EFFECTIVE_URL, &url);
    if (url != NULL) {
        c->effective_url = malloc(strlen(url) + 1);
        if (c->effective_url == NULL)
            c->code = CURLE_OUT_OF_MEMORY;
        else
            strcpy(c->effective_url, url);
    }
finished:
    rl_curl_stop(c);
    c->task = NULL;
    knurl_error_clear(error);
    return KNURL_OK;
}
static int result(lua_State *L, void *data, knurl_status status,
                  const knurl_error *error)
{
    struct rl_curl_call *c = data;
    if (status != KNURL_OK) {
        c->code = status == KNURL_TIMEOUT ? CURLE_OPERATION_TIMEDOUT
                                          : CURLE_ABORTED_BY_CALLBACK;
        snprintf(c->error, sizeof(c->error), "%s", error->message);
    }
    lua_newtable(L);
    lua_pushboolean(L, c->code == CURLE_OK && !c->callback_error);
    lua_setfield(L, -2, "ok");
    lua_pushinteger(L, c->code);
    lua_setfield(L, -2, "code");
    lua_pushstring(L, curl_easy_strerror((CURLcode)c->code));
    lua_setfield(L, -2, "code_name");
    lua_pushinteger(L, c->status);
    lua_setfield(L, -2, "status");
    lua_pushstring(L, c->effective_url);
    lua_setfield(L, -2, "effective_url");
    lua_pushlstring(L, c->head ? (const char *)c->head : "", c->header_length);
    lua_setfield(L, -2, "headers");
    lua_pushlstring(L, c->body ? (const char *)c->body : "", c->body_length);
    lua_setfield(L, -2, "body");
    lua_pushboolean(L, c->write_callback || c->download || c->json_write);
    lua_setfield(L, -2, "streamed");
    if (c->code != CURLE_OK || c->callback_error) {
        lua_pushstring(L, c->callback_error ? c->callback_message
                          : c->error[0]
                              ? c->error
                              : curl_easy_strerror((CURLcode)c->code));
        lua_setfield(L, -2, "error");
    }
    rl_curl_json_result(L, c);
    return 1;
}
static int perform(lua_State *L)
{
    struct rl_async *a = rl_async_current(L);
    struct rl_curl_call *c;
    knurl_lua_async_operation op;
    knurl_error error;
    int self, async = lua_toboolean(L, lua_upvalueindex(1));
    if (!lua_istable(L, 1))
        return luaL_error(L, "curl.perform requires an options table");
    if (in_callback(L))
        return luaL_error(L, "curl callbacks cannot start nested transfers");
    if (async && (a == NULL || !lua_isyieldable(L)))
        return luaL_error(L, "async.curl requires a yieldable Knurl route");
    c = lua_newuserdatauv(L, sizeof(*c), 1);
    memset(c, 0, sizeof(*c));
    c->options = c->records[0] = c->records[1] = LUA_NOREF;
    c->L = async ? a->call.server->L : L;
    c->owned = 1;
    self = lua_absindex(L, -1);
    luaL_setmetatable(L, RL_CURL_CALL);
    lua_pushboolean(L, 0);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &callback_key);
    lua_pushthread(L);
    lua_setiuservalue(L, self, 1);
    lua_pushvalue(L, 1);
    c->options = luaL_ref(L, LUA_REGISTRYINDEX);
    if (pthread_once(&curl_once, initialize_curl) != 0 ||
        curl_initialized != CURLE_OK || (c->easy = curl_easy_init()) == NULL)
        return luaL_error(L, "cannot initialize curl");
    if (rl_curl_config(L, c, &error) != KNURL_OK)
        return luaL_error(L, "%s", error.message);
    if (curl_easy_setopt(c->easy, CURLOPT_ERRORBUFFER, c->error) != CURLE_OK ||
        curl_easy_setopt(c->easy, CURLOPT_WRITEFUNCTION, body_write) !=
            CURLE_OK ||
        curl_easy_setopt(c->easy, CURLOPT_WRITEDATA, c) != CURLE_OK ||
        curl_easy_setopt(c->easy, CURLOPT_HEADERFUNCTION, head_write) !=
            CURLE_OK ||
        curl_easy_setopt(c->easy, CURLOPT_HEADERDATA, c) != CURLE_OK ||
        curl_easy_setopt(c->easy, CURLOPT_NOPROGRESS, 0L) != CURLE_OK ||
        curl_easy_setopt(c->easy, CURLOPT_XFERINFOFUNCTION, progress) !=
            CURLE_OK ||
        curl_easy_setopt(c->easy, CURLOPT_XFERINFODATA, c) != CURLE_OK)
        return luaL_error(L, "cannot configure curl callbacks");
    if (c->read_callback || c->input_path != NULL || c->json_read ||
        c->request_body != NULL) {
        if (curl_easy_setopt(c->easy, CURLOPT_READFUNCTION, body_read) !=
                CURLE_OK ||
            curl_easy_setopt(c->easy, CURLOPT_READDATA, c) != CURLE_OK)
            return luaL_error(L, "cannot configure curl upload");
    }
    if (c->json_read &&
        (curl_easy_setopt(c->easy, CURLOPT_SEEKFUNCTION, seek) != CURLE_OK ||
         curl_easy_setopt(c->easy, CURLOPT_SEEKDATA, c) != CURLE_OK))
        return luaL_error(L, "cannot configure curl upload replay");
    if (async) {
        memset(&op, 0, sizeof(op));
        op.work = execute;
        op.complete = result;
        op.timeout_ms = c->timeout_ms;
        op.timeout_disabled = c->timeout_ms == 0;
        return knurl_lua_async_run(L, &op, c, self);
    }
    (void)execute(NULL, c, &error);
    return result(L, c, KNURL_OK, &error);
}
static int version(lua_State *L)
{
    lua_pushstring(L, curl_version());
    return 1;
}
static int core(lua_State *L, int async)
{
    void *key = async ? &rl_async_curl_core_key : &rl_curl_core_key;
    lua_rawgetp(L, LUA_REGISTRYINDEX, key);
    if (!lua_isnil(L, -1))
        return 1;
    lua_pop(L, 1);
    luaL_newmetatable(L, RL_CURL_CALL);
    lua_pushcfunction(L, gc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushboolean(L, async);
    lua_pushcclosure(L, perform, 1);
    lua_setfield(L, -2, "perform");
    lua_pushcfunction(L, version);
    lua_setfield(L, -2, "version");
    lua_pushvalue(L, -1);
    lua_rawsetp(L, LUA_REGISTRYINDEX, key);
    return 1;
}
int rl_curl_core(lua_State *L) { return core(L, 0); }
int rl_async_curl_core(lua_State *L) { return core(L, 1); }
static int loader(lua_State *L, int async)
{
    void *key = async ? &rl_async_curl_facade_key : &rl_curl_facade_key;
    lua_rawgetp(L, LUA_REGISTRYINDEX, key);
    if (!lua_isnil(L, -1))
        return 1;
    lua_pop(L, 1);
    if (luaL_loadbufferx(L, (const char *)knurl_facade, sizeof(knurl_facade),
                         "@curl.lua", "t") != LUA_OK)
        return lua_error(L);
    core(L, async);
    lua_call(L, 1, 1);
    lua_pushvalue(L, -1);
    lua_rawsetp(L, LUA_REGISTRYINDEX, key);
    return 1;
}
int rl_curl_loader(lua_State *L) { return loader(L, 0); }
int rl_async_curl_loader(lua_State *L) { return loader(L, 1); }
