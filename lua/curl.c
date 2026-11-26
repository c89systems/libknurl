/* MIT License; synchronous curl binding adapted from Vectis.
 * Copyright (c) 2026 C89 Systems AB. */
#define _POSIX_C_SOURCE 200809L
#include "curl_internal.h"
#include <fcntl.h>
#include <unistd.h>

static char core_key, transfer_key;

static int release(lua_State *L)
{
    struct rl_curl_call *c = luaL_checkudata(L, 1, RL_CURL_CALL);
    struct rl_curl_mime_file *file;
    if (c->stopped)
        return 0;
    c->stopped = 1;
    if (c->easy != NULL)
        curl_easy_cleanup(c->easy);
    curl_slist_free_all(c->headers);
    curl_slist_free_all(c->recipients);
    if (c->mime != NULL)
        curl_mime_free(c->mime);
    while ((file = c->mime_files) != NULL) {
        c->mime_files = file->next;
        fclose(file->stream);
        free(file);
    }
    if (c->initialized)
        curl_global_cleanup();
    if (c->input != NULL)
        fclose(c->input);
    if (c->output != NULL)
        fclose(c->output);
    if (c->json_read)
        lonejson_curl_upload_cleanup(&c->upload);
    if (c->json_write)
        lonejson_curl_parse_cleanup(&c->parser);
    luaL_unref(L, LUA_REGISTRYINDEX, c->records[0]);
    luaL_unref(L, LUA_REGISTRYINDEX, c->records[1]);
    free(c->body);
    free(c->head);
    free(c->request_body);
    free(c->input_path);
    free(c->output_path);
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
        while (capacity < *used + count)
            capacity *= 2;
        next = realloc(*buffer, capacity);
        if (next == NULL)
            return 0;
        *buffer = next;
        *allocated = capacity;
    }
    if (count != 0)
        memcpy(*buffer + *used, bytes, count);
    *used += count;
    return 1;
}

enum callback_kind { WRITE_CHUNK, READ_CHUNK, TRANSFER_PROGRESS };
struct callback_call {
    enum callback_kind kind;
    char *bytes;
    size_t length, produced;
    curl_off_t progress[4];
    int accepted;
};

static void callback_failure(struct rl_curl_call *c, const char *message)
{
    c->callback_error = 1;
    snprintf(c->callback_message, sizeof(c->callback_message), "%s", message);
}

static int callback_trampoline(lua_State *L)
{
    static const char *const names[] = {"write", "read", "progress"};
    static const char *const fields[] = {"download_total", "downloaded",
                                         "upload_total", "uploaded"};
    struct callback_call *call = lua_touserdata(L, 1);
    const char *bytes;
    size_t length, i;
    rl_field(L, 2, names[call->kind]);
    switch (call->kind) {
    case WRITE_CHUNK:
        lua_pushlstring(L, call->bytes, call->length);
        break;
    case READ_CHUNK:
        lua_pushinteger(L, (lua_Integer)call->length);
        break;
    case TRANSFER_PROGRESS:
        lua_createtable(L, 0, 4);
        for (i = 0; i < RL_COUNT(fields); ++i) {
            lua_pushinteger(L, (lua_Integer)call->progress[i]);
            lua_setfield(L, -2, fields[i]);
        }
        break;
    }
    lua_call(L, 1, 2);
    if (!lua_isnil(L, -1)) {
        if (lua_type(L, -1) == LUA_TSTRING)
            return lua_error(L);
        return luaL_error(L, "curl callback returned an error");
    }
    if (call->kind != READ_CHUNK)
        call->accepted = lua_isboolean(L, -2) && lua_toboolean(L, -2);
    else if (lua_isnil(L, -2))
        call->accepted = 1;
    else if (lua_type(L, -2) == LUA_TSTRING) {
        bytes = lua_tolstring(L, -2, &length);
        if (length > 0 && length <= call->length) {
            memcpy(call->bytes, bytes, length);
            call->produced = length;
            call->accepted = 1;
        }
    }
    return 0;
}

/* The Lua application thread performs the transfer. Protect the Lua call so
 * callback errors unwind here before returning control to libcurl. */
static int invoke_callback(struct rl_curl_call *c, struct callback_call *call)
{
    lua_State *L = c->L;
    int status;
    if (!lua_checkstack(L, 3)) {
        callback_failure(c, "cannot reserve curl callback stack");
        return 0;
    }
    lua_pushcfunction(L, callback_trampoline);
    lua_pushlightuserdata(L, call);
    lua_pushvalue(L, 1);
    status = lua_pcall(L, 2, 0, 0);
    if (status != LUA_OK) {
        callback_failure(c, lua_type(L, -1) == LUA_TSTRING
                                ? lua_tostring(L, -1)
                                : "curl callback failed");
        lua_pop(L, 1);
        return 0;
    }
    if (!call->accepted)
        callback_failure(
            c, "curl callback rejected data or returned an invalid result");
    return call->accepted;
}

static size_t body_write(char *data, size_t size, size_t count, void *userdata)
{
    struct rl_curl_call *c = userdata;
    struct callback_call call;
    size_t length;
    if (c->stopped || (size != 0 && count > (size_t)-1 / size))
        return 0;
    length = size * count;
    if (c->output != NULL)
        return fwrite(data, 1, length, c->output);
    if (c->json_write) {
        if (length > RL_CURL_BODY_LIMIT - c->received) {
            callback_failure(c, "curl JSON response exceeds 8 MiB");
            return 0;
        }
        c->received += length;
        if (lonejson_curl_write_callback(data, 1, length, &c->parser) == length)
            return length;
        callback_failure(c, c->parser.error.message[0]
                                ? c->parser.error.message
                                : "cannot parse curl JSON response");
        return 0;
    }
    if (c->write_callback) {
        memset(&call, 0, sizeof(call));
        call.kind = WRITE_CHUNK;
        call.bytes = data;
        call.length = length;
        return invoke_callback(c, &call) ? length : 0;
    }
    if (!append(&c->body, &c->body_length, &c->body_capacity, data, length,
                RL_CURL_BODY_LIMIT)) {
        callback_failure(
            c, "curl buffered response exceeds 8 MiB or allocation failed");
        return 0;
    }
    return length;
}

static size_t head_write(char *data, size_t size, size_t count, void *userdata)
{
    struct rl_curl_call *c = userdata;
    if (c->stopped || (size != 0 && count > (size_t)-1 / size))
        return 0;
    if (!append(&c->head, &c->header_length, &c->header_capacity, data,
                size * count, RL_CURL_HEADER_LIMIT)) {
        callback_failure(
            c, "curl response headers exceed 64 KiB or allocation failed");
        return 0;
    }
    return size * count;
}

static size_t body_read(char *data, size_t size, size_t count, void *userdata)
{
    struct rl_curl_call *c = userdata;
    struct callback_call call;
    size_t capacity;
    if (c->stopped || (size != 0 && count > (size_t)-1 / size))
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
    if (c->json_read)
        return lonejson_curl_read_callback(data, 1, capacity, &c->upload);
    if (!c->read_callback)
        return 0;
    memset(&call, 0, sizeof(call));
    call.kind = READ_CHUNK;
    call.bytes = data;
    call.length = capacity > 65536 ? 65536 : capacity;
    return invoke_callback(c, &call) ? call.produced : CURL_READFUNC_ABORT;
}

int rl_curl_file_seek(void *data, curl_off_t offset, int origin)
{
    return (curl_off_t)(off_t)offset == offset &&
                   fseeko(data, (off_t)offset, origin) == 0
               ? CURL_SEEKFUNC_OK
               : CURL_SEEKFUNC_FAIL;
}

static int body_seek(void *data, curl_off_t offset, int origin)
{
    struct rl_curl_call *c = data;
    curl_off_t base = 0, length = (curl_off_t)c->request_length;
    if (c->input != NULL)
        return rl_curl_file_seek(c->input, offset, origin);
    if (origin == SEEK_CUR)
        base = (curl_off_t)c->request_offset;
    else if (origin == SEEK_END)
        base = length;
    else if (origin != SEEK_SET)
        return CURL_SEEKFUNC_FAIL;
    if (offset < -base || offset > length - base)
        return CURL_SEEKFUNC_FAIL;
    c->request_offset = (size_t)(base + offset);
    return CURL_SEEKFUNC_OK;
}

static int progress(void *data, curl_off_t dt, curl_off_t dn, curl_off_t ut,
                    curl_off_t un)
{
    struct rl_curl_call *c = data;
    struct callback_call call;
    if (c->stopped)
        return 1;
    memset(&call, 0, sizeof(call));
    call.kind = TRANSFER_PROGRESS;
    call.progress[0] = dt;
    call.progress[1] = dn;
    call.progress[2] = ut;
    call.progress[3] = un;
    return invoke_callback(c, &call) ? 0 : 1;
}

int rl_curl_open_file(const char *path, int flags, struct stat *info)
{
    /* Nonblocking open lets us reject FIFOs before any transfer starts. */
    int fd = open(path, flags | O_NONBLOCK | O_CLOEXEC, 0666);
    if (fd < 0)
        return -1;
    if (fstat(fd, info) == 0 && S_ISREG(info->st_mode) &&
        fcntl(fd, F_SETFL, 0) == 0)
        return fd;
    close(fd);
    return -1;
}

static int same_file(const struct stat *a, const struct stat *b)
{
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino;
}

static int output_aliases_upload(const struct rl_curl_call *c,
                                 const struct stat *input,
                                 const struct stat *output)
{
    const struct rl_curl_mime_file *file;
    if (c->input != NULL && same_file(input, output))
        return 1;
    for (file = c->mime_files; file != NULL; file = file->next)
        if (same_file(&file->info, output))
            return 1;
    return 0;
}

static void execute(struct rl_curl_call *c)
{
    struct stat input_info, output_info;
    int fd;
    if (c->input_path != NULL) {
        fd = rl_curl_open_file(c->input_path, O_RDONLY, &input_info);
        if (fd < 0 || (c->input = fdopen(fd, "rb")) == NULL) {
            if (fd >= 0)
                close(fd);
            c->code = CURLE_READ_ERROR;
            snprintf(c->error, sizeof(c->error),
                     "curl upload_path must be a readable regular file");
            return;
        }
        c->code = (int)curl_easy_setopt(c->easy, CURLOPT_INFILESIZE_LARGE,
                                        (curl_off_t)input_info.st_size);
        if (c->code != CURLE_OK)
            return;
    }
    if (c->output_path != NULL) {
        fd =
            rl_curl_open_file(c->output_path, O_WRONLY | O_CREAT, &output_info);
        if (fd >= 0 && output_aliases_upload(c, &input_info, &output_info)) {
            close(fd);
            c->code = CURLE_WRITE_ERROR;
            snprintf(c->error, sizeof(c->error),
                     "curl upload sources and download_path must refer to "
                     "different files");
            return;
        }
        if (fd < 0 || (c->output = fdopen(fd, "wb")) == NULL ||
            ftruncate(fd, 0) != 0) {
            if (fd >= 0 && c->output == NULL)
                close(fd);
            c->code = CURLE_WRITE_ERROR;
            snprintf(c->error, sizeof(c->error),
                     "curl download_path must be a writable regular file");
            return;
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
}

static int result(lua_State *L, struct rl_curl_call *c)
{
    char *url = NULL;
    (void)curl_easy_getinfo(c->easy, CURLINFO_RESPONSE_CODE, &c->status);
    (void)curl_easy_getinfo(c->easy, CURLINFO_EFFECTIVE_URL, &url);
    lua_createtable(L, 0, 8);
    lua_pushboolean(L, c->code == CURLE_OK && !c->callback_error);
    lua_setfield(L, -2, "ok");
    lua_pushinteger(L, c->code);
    lua_setfield(L, -2, "code");
    lua_pushstring(L, curl_easy_strerror((CURLcode)c->code));
    lua_setfield(L, -2, "code_name");
    lua_pushinteger(L, c->status);
    lua_setfield(L, -2, "status");
    lua_pushstring(L, url);
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
    struct rl_curl_call *c;
    knurl_error error;
    int busy;
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_rawgetp(L, LUA_REGISTRYINDEX, &transfer_key);
    busy = lua_toboolean(L, -1);
    lua_pop(L, 1);
    if (busy)
        return luaL_error(L, "curl callbacks cannot start nested transfers");
    c = lua_newuserdatauv(L, sizeof(*c), 0);
    memset(c, 0, sizeof(*c));
    c->records[0] = c->records[1] = LUA_NOREF;
    c->L = L;
    luaL_setmetatable(L, RL_CURL_CALL);
    lua_toclose(L, -1);
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
        return luaL_error(L, "cannot initialize curl");
    c->initialized = 1;
    c->easy = curl_easy_init();
    if (c->easy == NULL)
        return luaL_error(L, "cannot allocate curl transfer");
    if (rl_curl_config(L, c, &error) != KNURL_OK)
        return luaL_error(L, "%s", error.message);
    if (curl_easy_setopt(c->easy, CURLOPT_ERRORBUFFER, c->error) != CURLE_OK ||
        curl_easy_setopt(c->easy, CURLOPT_WRITEFUNCTION, body_write) !=
            CURLE_OK ||
        curl_easy_setopt(c->easy, CURLOPT_WRITEDATA, c) != CURLE_OK ||
        curl_easy_setopt(c->easy, CURLOPT_HEADERFUNCTION, head_write) !=
            CURLE_OK ||
        curl_easy_setopt(c->easy, CURLOPT_HEADERDATA, c) != CURLE_OK)
        return luaL_error(L, "cannot configure curl response callbacks");
    if (c->progress_callback &&
        (curl_easy_setopt(c->easy, CURLOPT_NOPROGRESS, 0L) != CURLE_OK ||
         curl_easy_setopt(c->easy, CURLOPT_XFERINFOFUNCTION, progress) !=
             CURLE_OK ||
         curl_easy_setopt(c->easy, CURLOPT_XFERINFODATA, c) != CURLE_OK))
        return luaL_error(L, "cannot configure curl progress callback");
    if (curl_easy_setopt(c->easy, CURLOPT_READFUNCTION, body_read) !=
            CURLE_OK ||
        curl_easy_setopt(c->easy, CURLOPT_READDATA, c) != CURLE_OK)
        return luaL_error(L, "cannot configure curl upload");
    if ((c->input_path != NULL || c->request_body != NULL) &&
        (curl_easy_setopt(c->easy, CURLOPT_SEEKFUNCTION, body_seek) !=
             CURLE_OK ||
         curl_easy_setopt(c->easy, CURLOPT_SEEKDATA, c) != CURLE_OK))
        return luaL_error(L, "cannot configure curl upload replay");
    lua_pushboolean(L, 1);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &transfer_key);
    execute(c);
    lua_pushboolean(L, 0);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &transfer_key);
    return result(L, c);
}

static int version(lua_State *L)
{
    lua_pushstring(L, curl_version());
    return 1;
}

int rl_curl_core(lua_State *L)
{
    static const luaL_Reg methods[] = {
        {"perform", perform}, {"version", version}, {NULL, NULL}};
    lua_rawgetp(L, LUA_REGISTRYINDEX, &core_key);
    if (!lua_isnil(L, -1))
        return 1;
    lua_pop(L, 1);
    luaL_newmetatable(L, RL_CURL_CALL);
    lua_pushcfunction(L, release);
    lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, release);
    lua_setfield(L, -2, "__close");
    lua_pop(L, 1);
    luaL_newlib(L, methods);
    lua_pushvalue(L, -1);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &core_key);
    return 1;
}
