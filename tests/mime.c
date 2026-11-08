#include <rotmos/rotmos.h>
#include <stdio.h>
#include <string.h>

#define CHECK(t)                                                               \
    do {                                                                       \
        if (!(t)) {                                                            \
            fprintf(stderr, "mime %d: %s\n", __LINE__, #t);                    \
            return 1;                                                          \
        }                                                                      \
    } while (0)

static int content_types(void)
{
    static const char *valid[] = {"Application/Vnd.Example+json",
                                  "text/plain; charset=utf-8",
                                  "text/plain ;\tcharset=\"utf-8\"; empty=\"\"",
                                  "text/plain; name=\"a;b,c=1\\\"q\\\\x\"",
                                  "text/plain;;",
                                  "\ttext/plain \t",
                                  "application/x!#$%&'*+-.^_`|~",
                                  "text/plain; label=\"\200\377\""};
    static const char *invalid[] = {"text/plain; charset",
                                    "text/plain; =utf-8",
                                    "text/plain; charset =utf-8",
                                    "text/plain; charset= utf-8",
                                    "text/plain; charset=",
                                    "text/plain; charset=\"unfinished",
                                    "text/plain; charset=\"utf-8\"junk",
                                    "text/plain; name=\"a\\\r\"",
                                    "text/plain; name=\"a\\\n\"",
                                    "text/plain; name=\"\177\"",
                                    "text/plain; name=\"a\\",
                                    "text/plain; name=value, text/html",
                                    "text/plain (comment)",
                                    "text/plain,application/json",
                                    "text /plain",
                                    "text/ plain",
                                    "text/\200",
                                    "text/plain; \200=value",
                                    "text/plain; name=\200",
                                    "text/plain\r\nInjected: yes",
                                    "text/plain\n",
                                    "*/*"};
    rotmos_reply *reply;
    rotmos_error error;
    rotmos_bytes current;
    rotmos_header saved_header;
    char value[80];
    size_t i;
    int c, context, accepted;
    CHECK(rotmos_reply_new(&reply, &error) == ROTMOS_OK);
    for (i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i) {
        CHECK(rotmos_reply_text(reply, 200, valid[i], "body", &error) ==
              ROTMOS_OK);
        CHECK(rotmos_reply_header_set(reply, "Content-Type", valid[i],
                                      &error) == ROTMOS_OK);
    }
    CHECK(rotmos_reply_text(reply, 200, "text/plain", "body", &error) ==
          ROTMOS_OK);
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        CHECK(rotmos_reply_text(reply, 200, invalid[i], "body", &error) ==
              ROTMOS_INVALID);
        CHECK(rotmos_reply_header_set(reply, "cOnTeNt-TyPe", invalid[i],
                                      &error) == ROTMOS_INVALID);
        current = rotmos_reply_content_type(reply);
        CHECK(current.length == 10 &&
              memcmp(current.data, "text/plain", 10) == 0);
        CHECK(rotmos_reply_header_count(reply) == 1 &&
              rotmos_reply_header_at(reply, 0, &saved_header, &error) ==
                  ROTMOS_OK);
        CHECK(saved_header.value.length == strlen(valid[7]) &&
              memcmp(saved_header.value.data, valid[7], strlen(valid[7])) == 0);
    }
    /* Independent RFC byte classes, exercised through the public API. NUL is
     * the C string terminator; Lua length-aware conversion rejects embedded
     * NUL. */
    for (context = 0; context < 6; ++context)
        for (c = 1; c <= 255; ++c) {
            if (context < 4) {
                accepted = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') ||
                           strchr("!#$%&'*+-.^_`|~", c) != NULL;
                if (context == 0)
                    sprintf(value, "x%cx/plain", c);
                else if (context == 1)
                    sprintf(value, "text/x%cx", c);
                else if (context == 2)
                    sprintf(value, "text/plain;x%cx=value", c);
                else
                    sprintf(value, "text/plain;p=x%cx", c);
            } else {
                accepted = c == 9 || (c >= 32 && c != 127);
                if (context == 4) {
                    accepted = accepted && c != '"';
                    sprintf(value, "text/plain;p=\"x%cx\"", c);
                } else
                    sprintf(value, "text/plain;p=\"x\\%cx\"", c);
            }
            CHECK(rotmos_reply_text(reply, 200, value, "body", &error) ==
                  (accepted ? ROTMOS_OK : ROTMOS_INVALID));
        }
    CHECK(rotmos_reply_destroy(reply, &error) == ROTMOS_OK);
    return 0;
}

int main(void)
{
    static const rotmos_mime_type examples[] = {
        {"song.MP3", "audio/mpeg"},
        {"song.flac", "audio/flac"},
        {"song.m4a", "audio/mp4"},
        {"voice.opus", "audio/ogg"},
        {"song.aac", "audio/aac"},
        {"song.wav", "audio/wav"},
        {"movie.mp4", "video/mp4"},
        {"movie.webm", "video/webm"},
        {"movie.mov", "video/quicktime"},
        {"movie.mkv", "video/matroska"},
        {"list.m3u8", "application/vnd.apple.mpegurl"},
        {"part.ts", "video/mp2t"},
        {"part.m4s", "video/iso.segment"},
        {"stream.mpd", "application/dash+xml"},
        {"sub.vtt", "text/vtt; charset=utf-8"},
        {"image.avif", "image/avif"},
        {"image.apng", "image/apng"},
        {"image.tiff", "image/tiff"},
        {"image.heic", "image/heic"},
        {"image.webp", "image/webp"},
        {"mesh.glb", "model/gltf-binary"},
        {"font.ttf", "font/ttf"},
        {"font.otf", "font/otf"},
        {"font.woff2", "font/woff2"},
        {"archive.tar.gz", "application/gzip"},
        {"archive.zip", "application/zip"},
        {"archive.rar", "application/vnd.rar"},
        {"archive.zst", "application/zstd"},
        {"book.epub", "application/epub+zip"},
        {"doc.docx",
         "application/"
         "vnd.openxmlformats-officedocument.wordprocessingml.document"},
        {"sheet.ods", "application/vnd.oasis.opendocument.spreadsheet"},
        {"site.webmanifest", "application/manifest+json"},
        {"doc.yaml", "application/yaml"},
        {"README.MarkDown", "text/markdown; charset=utf-8"},
        {"app.CJS", "text/javascript; charset=utf-8"},
        {"app.wasm", "application/wasm"},
        {"directory.mp3/file", "application/octet-stream"},
        {"directory.mp3\\file", "application/octet-stream"},
        {"file.unknown", "application/octet-stream"},
        {".mp3", "application/octet-stream"},
        {"file.", "application/octet-stream"}};
    static const rotmos_mime_type invalid[] = {
        {NULL, "text/plain"},
        {"", "text/plain"},
        {".", "text/plain"},
        {"a/b", "text/plain"},
        {"a\\b", "text/plain"},
        {"tar.gz", "text/plain"},
        {"a b", "text/plain"},
        {"x?", "text/plain"},
        {"\xff", "text/plain"},
        {"txt", NULL},
        {"txt", ""},
        {"txt", "plain"},
        {"txt", "text/"},
        {"txt", "/plain"},
        {"txt", "text/plain\r\nInjected: yes"},
        {"txt", "text/plain\177"},
        {"txt", "text/*"},
        {"txt", "text/plain garbage"}};
    rotmos_server_config config;
    rotmos_listener_config listener;
    rotmos_server_effective effective;
    rotmos_server *server, *other, *rejected;
    rotmos_static_config route;
    rotmos_error error;
    rotmos_mime_type types[2];
    char ext[] = ".MP3", type[] = "audio/x-custom; codecs=fixture";
    size_t i, count;
    CHECK(content_types() == 0);
    for (i = 0; i < sizeof(examples) / sizeof(examples[0]); ++i)
        CHECK(strcmp(rotmos_static_content_type(examples[i].extension),
                     examples[i].content_type) == 0);
    CHECK(strcmp(rotmos_static_content_type(NULL),
                 "application/octet-stream") == 0);
    CHECK(strcmp(rotmos_server_content_type(NULL, "song.mp3"), "audio/mpeg") ==
          0);
    memset(&config, 0xa5, sizeof(config));
    rotmos_server_config_init(&config);
    CHECK(config.http.mime_types == NULL && config.http.mime_type_count == 0);
    rotmos_listener_config_init(&listener);
    listener.bind = "127.0.0.1";
    config.listeners = &listener;
    config.listener_count = config.workers.count = 1;
    config.http.backend_quiet = 1;
    CHECK(rotmos_server_new(&config, &other, &error) == ROTMOS_OK);
    types[0].extension = ext;
    types[0].content_type = type;
    config.http.mime_types = types;
    config.http.mime_type_count = 1;
    CHECK(rotmos_server_new(&config, &server, &error) == ROTMOS_OK);
    ext[1] = 'X';
    type[0] = 'X';
    CHECK(strcmp(rotmos_server_content_type(server, "dir/track.mP3"),
                 "audio/x-custom; codecs=fixture") == 0);
    CHECK(strcmp(rotmos_server_content_type(other, "dir/track.mp3"),
                 "audio/mpeg") == 0);
    CHECK(rotmos_server_effective_config(server, &effective, &error) ==
          ROTMOS_OK);
    CHECK(effective.config.http.mime_type_count == 1 &&
          strcmp(effective.config.http.mime_types[0].extension, "mp3") == 0);
    CHECK(strcmp(rotmos_server_content_type(server, "movie.mp4"),
                 "video/mp4") == 0);
    CHECK(strcmp(rotmos_server_content_type(server, "unknown"),
                 "application/octet-stream") == 0);
    rotmos_static_config_init(&route);
    route.path = "/files";
    route.root_dir = ".";
    route.mime_type_count = 1;
    count = rotmos_router_route_count(rotmos_server_router(server));
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        config.http.mime_types = &invalid[i];
        CHECK(rotmos_server_new(&config, &rejected, &error) == ROTMOS_INVALID);
        CHECK(rejected == NULL && strcmp(error.operation, "mime_types") == 0);
        route.mime_types = &invalid[i];
        CHECK(rotmos_router_static_directory(rotmos_server_router(server),
                                             &route, NULL,
                                             &error) == ROTMOS_INVALID);
        CHECK(rotmos_router_route_count(rotmos_server_router(server)) == count);
        if (invalid[i].content_type != NULL && invalid[i].extension != NULL &&
            strcmp(invalid[i].extension, "txt") == 0) {
            route.mime_type_count = 0;
            route.content_type = invalid[i].content_type;
            CHECK(rotmos_router_static_directory(rotmos_server_router(server),
                                                 &route, NULL,
                                                 &error) == ROTMOS_INVALID);
            route.content_type = NULL;
            route.not_found_content_type = invalid[i].content_type;
            CHECK(rotmos_router_static_directory(rotmos_server_router(server),
                                                 &route, NULL,
                                                 &error) == ROTMOS_INVALID);
            route.not_found_content_type = NULL;
            route.mime_type_count = 1;
        }
    }
    types[0].extension = "mp3";
    types[0].content_type = "audio/mpeg";
    types[1].extension = ".MP3";
    types[1].content_type = "audio/mp4";
    config.http.mime_types = route.mime_types = types;
    config.http.mime_type_count = route.mime_type_count = 2;
    CHECK(rotmos_server_new(&config, &rejected, &error) == ROTMOS_INVALID);
    CHECK(rotmos_router_static_directory(rotmos_server_router(server), &route,
                                         NULL, &error) == ROTMOS_INVALID);
    config.http.mime_types = route.mime_types = NULL;
    CHECK(rotmos_server_new(&config, &rejected, &error) == ROTMOS_INVALID);
    CHECK(rotmos_router_static_directory(rotmos_server_router(server), &route,
                                         NULL, &error) == ROTMOS_INVALID);
    config.http.mime_types = route.mime_types = types;
    config.http.mime_type_count = route.mime_type_count = (size_t)-1;
    CHECK(rotmos_server_new(&config, &rejected, &error) == ROTMOS_INVALID);
    CHECK(rotmos_router_static_directory(rotmos_server_router(server), &route,
                                         NULL, &error) == ROTMOS_INVALID);
    CHECK(rotmos_router_route_count(rotmos_server_router(server)) == count);
    CHECK(rotmos_server_destroy(server, &error) == ROTMOS_OK);
    CHECK(rotmos_server_destroy(other, &error) == ROTMOS_OK);
    return 0;
}
