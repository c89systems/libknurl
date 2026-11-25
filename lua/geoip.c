#define _POSIX_C_SOURCE 200809L
#include "binding.h"
#include <curl/curl.h>
#include <zlib.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define GEOIP_FILE "dbip-country-lite.mmdb"
#define GEOIP_LIMIT (64UL * 1024 * 1024)

struct download {
    z_stream gzip;
    FILE *file;
    unsigned char chunk[8192];
    const char *error;
    size_t received, written;
    int finished;
};

static knurl_status system_error(knurl_error *error, const char *message)
{
    int saved = errno;
    rl_fail(error, KNURL_IO, message);
    error->source = KNURL_ERROR_SYSTEM;
    error->native_code = saved;
    snprintf(error->detail, sizeof(error->detail), "%s", strerror(saved));
    return KNURL_IO;
}

static int make_directory(char *path)
{
    struct stat info;
    char *end = path + 1;
    for (;;) {
        end += strcspn(end, "/");
        if (end > path + 1) {
            char separator = *end;
            *end = '\0';
            if (mkdir(path, 0700) != 0) {
                if (errno != EEXIST || stat(path, &info) != 0)
                    return 0;
                if (!S_ISDIR(info.st_mode)) {
                    errno = ENOTDIR;
                    return 0;
                }
            }
            *end = separator;
        }
        if (*end == '\0')
            return 1;
        ++end;
    }
}

static knurl_status validate_database(const char *path, knurl_error *error)
{
    knurl_mmdb *db = NULL;
    knurl_status status = knurl_mmdb_open(path, NULL, &db, error);
    return status == KNURL_OK ? knurl_mmdb_close(db, error) : status;
}

static size_t decompress(char *input, size_t size, size_t count, void *data)
{
    struct download *reader = data;
    size_t length, bytes;
    int status;
    if (size != 0 && count > UINT_MAX / size)
        return 0;
    length = size * count;
    if (length == 0)
        return 0;
    if (reader->finished || length > GEOIP_LIMIT - reader->received) {
        reader->error = "extra or oversized GeoIP download data";
        return 0;
    }
    reader->received += length;
    reader->gzip.next_in = (Bytef *)input;
    reader->gzip.avail_in = (uInt)length;
    do {
        reader->gzip.next_out = reader->chunk;
        reader->gzip.avail_out = sizeof(reader->chunk);
        status = inflate(&reader->gzip, Z_NO_FLUSH);
        bytes = sizeof(reader->chunk) - reader->gzip.avail_out;
        if (status == Z_BUF_ERROR && reader->gzip.avail_in == 0 && bytes == 0)
            break;
        if ((status != Z_OK && status != Z_STREAM_END) ||
            (status == Z_STREAM_END && reader->gzip.avail_in != 0)) {
            reader->error = "invalid GeoIP gzip data";
            return 0;
        }
        if (bytes > GEOIP_LIMIT - reader->written) {
            reader->error = "GeoIP database exceeds 64 MiB";
            return 0;
        }
        if (fwrite(reader->chunk, 1, bytes, reader->file) != bytes) {
            reader->error = "cannot write GeoIP cache file";
            return 0;
        }
        reader->written += bytes;
        if (status == Z_STREAM_END) {
            reader->finished = 1;
            break;
        }
    } while (reader->gzip.avail_in != 0 || reader->gzip.avail_out == 0);
    return length;
}

static int configure_download(CURL *easy, struct download *reader,
                              const char *url, long timeout, char *message)
{
    const char *ca = knurl_tls_system_ca_file();
    const char *dir = getenv("SSL_CERT_DIR");
    return curl_easy_setopt(easy, CURLOPT_URL, url) == CURLE_OK &&
           curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "http,https") ==
               CURLE_OK &&
           curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L) == CURLE_OK &&
           curl_easy_setopt(easy, CURLOPT_FAILONERROR, 1L) == CURLE_OK &&
           curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, timeout) == CURLE_OK &&
           curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, 10000L) ==
               CURLE_OK &&
           curl_easy_setopt(easy, CURLOPT_CAINFO, ca) == CURLE_OK &&
           curl_easy_setopt(easy, CURLOPT_CAPATH, dir && *dir ? dir : NULL) ==
               CURLE_OK &&
           curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, decompress) ==
               CURLE_OK &&
           curl_easy_setopt(easy, CURLOPT_WRITEDATA, reader) == CURLE_OK &&
           curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, message) == CURLE_OK;
}

static knurl_status transfer(const char *url, long timeout, FILE *file,
                             long *response, knurl_error *error)
{
    struct download *reader;
    CURL *easy;
    CURLcode code;
    char message[CURL_ERROR_SIZE];
    knurl_status status;
    *response = 0;
    memset(message, 0, sizeof(message));
    reader = calloc(1, sizeof(*reader));
    if (reader == NULL)
        return rl_fail(error, KNURL_NOMEM, "allocating GeoIP download buffer");
    reader->file = file;
    if (inflateInit2(&reader->gzip, MAX_WBITS + 16) != Z_OK) {
        free(reader);
        return rl_fail(error, KNURL_BACKEND, "initializing GeoIP gzip decoder");
    }
    code = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (code == CURLE_OK) {
        easy = curl_easy_init();
        code = CURLE_FAILED_INIT;
        if (easy != NULL) {
            if (configure_download(easy, reader, url, timeout, message)) {
                code = curl_easy_perform(easy);
                (void)curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, response);
            }
            curl_easy_cleanup(easy);
        }
        curl_global_cleanup();
    }
    if (*response != 0 && *response != 200) {
        status =
            rl_fail(error, KNURL_IO, "GeoIP download returned an HTTP error");
        snprintf(error->detail, sizeof(error->detail), "HTTP %ld", *response);
    } else if (reader->error != NULL) {
        status = rl_fail(error, KNURL_IO, reader->error);
    } else if (code != CURLE_OK) {
        status = rl_fail(
            error, code == CURLE_OPERATION_TIMEDOUT ? KNURL_TIMEOUT : KNURL_IO,
            "GeoIP download failed");
        snprintf(error->detail, sizeof(error->detail), "%s",
                 *message ? message : curl_easy_strerror(code));
    } else if (*response != 200 || !reader->finished) {
        status = rl_fail(error, KNURL_IO, "incomplete GeoIP gzip download");
    } else {
        status = KNURL_OK;
    }
    (void)inflateEnd(&reader->gzip);
    free(reader);
    return status;
}

static knurl_status fetch(const char *url, long timeout, const char *path,
                          char *temporary, size_t capacity, long *response,
                          knurl_error *error)
{
    FILE *file;
    knurl_status status;
    int fd;
    snprintf(temporary, capacity, "%s.XXXXXX", path);
    fd = mkstemp(temporary);
    if (fd < 0)
        return system_error(error, "creating GeoIP cache file failed");
    file = fdopen(fd, "wb");
    if (file == NULL) {
        status = system_error(error, "opening GeoIP cache file failed");
        close(fd);
    } else {
        status = transfer(url, timeout, file, response, error);
        if (status == KNURL_OK && (fflush(file) != 0 || fsync(fd) != 0))
            status = system_error(error, "writing GeoIP cache file failed");
        if (fclose(file) != 0 && status == KNURL_OK)
            status = system_error(error, "closing GeoIP cache file failed");
    }
    if (status == KNURL_OK)
        status = validate_database(temporary, error);
    if (status == KNURL_OK && link(temporary, path) != 0) {
        if (errno == EEXIST)
            status = validate_database(path, error);
        else
            status = system_error(error, "publishing GeoIP cache file failed");
    }
    (void)unlink(temporary);
    return status;
}

static int country_database(lua_State *L)
{
    static const char *const fields[] = {"cache_dir", "url", "timeout_ms"};
    static const char format[] =
        "https://download.db-ip.com/free/dbip-country-lite-%04d-%02d.mmdb.gz";
    const char *directory = NULL, *path, *url = NULL, *home;
    char release[128], *temporary, *mkdir_path;
    knurl_error error;
    knurl_status status;
    struct stat info;
    struct tm date;
    time_t now;
    size_t timeout = 30000, capacity;
    long response = 0;
    int year, month;

    if (!lua_isnoneornil(L, 1)) {
        if (!rl_fields(L, 1, fields, RL_COUNT(fields), &error))
            return rl_error(L, &error);
        if (!rl_field_string(L, 1, "cache_dir", &directory) ||
            !rl_field_string(L, 1, "url", &url) ||
            !rl_field_size(L, 1, "timeout_ms", &timeout) || timeout == 0 ||
            timeout > LONG_MAX)
            return rl_result(L,
                             rl_fail(&error, KNURL_INVALID,
                                     "invalid GeoIP download options"),
                             &error);
    }
    if (directory == NULL) {
        directory = getenv("XDG_CACHE_HOME");
        if (directory != NULL && directory[0] == '/')
            directory = lua_pushfstring(L, "%s/knurl", directory);
        else {
            home = getenv("HOME");
            if (home != NULL && home[0] == '/')
                directory = lua_pushfstring(L, "%s/.cache/knurl", home);
            else
                directory = NULL;
        }
    }
    if (directory == NULL || directory[0] != '/')
        return rl_result(
            L,
            rl_fail(
                &error, KNURL_INVALID,
                "GeoIP requires an absolute cache_dir, XDG_CACHE_HOME or HOME"),
            &error);
    path = lua_pushfstring(L, "%s/%s", directory, GEOIP_FILE);
    if (stat(path, &info) == 0) {
        status = validate_database(path, &error);
    } else if (errno != ENOENT) {
        status = system_error(&error, "checking the GeoIP cache failed");
    } else {
        mkdir_path = lua_newuserdatauv(L, strlen(directory) + 1, 0);
        strcpy(mkdir_path, directory);
        if (!make_directory(mkdir_path))
            return rl_result(
                L,
                system_error(&error,
                             "creating the GeoIP cache directory failed"),
                &error);
        capacity = strlen(path) + sizeof(".XXXXXX");
        temporary = lua_newuserdatauv(L, capacity, 0);
        if (url != NULL) {
            status = fetch(url, (long)timeout, path, temporary, capacity,
                           &response, &error);
        } else {
            now = time(NULL);
            if (now == (time_t)-1 || gmtime_r(&now, &date) == NULL ||
                date.tm_year < 100 || date.tm_year > 8099)
                return rl_result(
                    L,
                    rl_fail(&error, KNURL_STATE,
                            "cannot determine the current GeoIP release month"),
                    &error);
            year = date.tm_year + 1900;
            month = date.tm_mon + 1;
            snprintf(release, sizeof(release), format, year, month);
            status = fetch(release, (long)timeout, path, temporary, capacity,
                           &response, &error);
            if (response == 404) {
                year -= month == 1;
                month = month == 1 ? 12 : month - 1;
                snprintf(release, sizeof(release), format, year, month);
                status = fetch(release, (long)timeout, path, temporary,
                               capacity, &response, &error);
            }
        }
    }
    if (status != KNURL_OK)
        return rl_error(L, &error);
    lua_pushstring(L, path);
    return 1;
}

void rl_geoip_open(lua_State *L)
{
    lua_pushcfunction(L, country_database);
    lua_setfield(L, -2, "country_database");
}
