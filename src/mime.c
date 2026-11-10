#include "server_internal.h"
#include "mime_internal.h"
#include <stdlib.h>
#include <string.h>

/* Registered media types where available; conventional types for formats
 * without one. This is extension inference, never content sniffing. */
static const knurl_mime_type defaults[] = {
    {"3g2", "video/3gpp2"},
    {"3gp", "video/3gpp"},
    {"7z", "application/x-7z-compressed"},
    {"aac", "audio/aac"},
    {"abw", "application/x-abiword"},
    {"aif", "audio/aiff"},
    {"aiff", "audio/aiff"},
    {"apng", "image/apng"},
    {"arc", "application/x-freearc"},
    {"atom", "application/atom+xml"},
    {"avif", "image/avif"},
    {"avi", "video/x-msvideo"},
    {"azw", "application/vnd.amazon.ebook"},
    {"bin", "application/octet-stream"},
    {"bmp", "image/bmp"},
    {"br", "application/octet-stream"},
    {"bz", "application/x-bzip"},
    {"bz2", "application/x-bzip2"},
    {"c", "text/plain; charset=utf-8"},
    {"cc", "text/plain; charset=utf-8"},
    {"cer", "application/pkix-cert"},
    {"cjs", "text/javascript; charset=utf-8"},
    {"cpp", "text/plain; charset=utf-8"},
    {"css", "text/css; charset=utf-8"},
    {"csv", "text/csv; charset=utf-8"},
    {"deb", "application/vnd.debian.binary-package"},
    {"doc", "application/msword"},
    {"docx",
     "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
    {"eot", "application/vnd.ms-fontobject"},
    {"epub", "application/epub+zip"},
    {"flac", "audio/flac"},
    {"flv", "video/x-flv"},
    {"gif", "image/gif"},
    {"glb", "model/gltf-binary"},
    {"gltf", "model/gltf+json"},
    {"gz", "application/gzip"},
    {"h", "text/plain; charset=utf-8"},
    {"heic", "image/heic"},
    {"heif", "image/heif"},
    {"hpp", "text/plain; charset=utf-8"},
    {"htm", "text/html; charset=utf-8"},
    {"html", "text/html; charset=utf-8"},
    {"ico", "image/vnd.microsoft.icon"},
    {"ics", "text/calendar; charset=utf-8"},
    {"jar", "application/java-archive"},
    {"jfif", "image/jpeg"},
    {"jpeg", "image/jpeg"},
    {"jpg", "image/jpeg"},
    {"js", "text/javascript; charset=utf-8"},
    {"json", "application/json"},
    {"jsonld", "application/ld+json"},
    {"jxl", "image/jxl"},
    {"log", "text/plain; charset=utf-8"},
    {"lua", "text/plain; charset=utf-8"},
    {"m3u", "audio/x-mpegurl"},
    {"m3u8", "application/vnd.apple.mpegurl"},
    {"m4a", "audio/mp4"},
    {"m4b", "audio/mp4"},
    {"m4s", "video/iso.segment"},
    {"m4v", "video/mp4"},
    {"map", "application/json"},
    {"markdown", "text/markdown; charset=utf-8"},
    {"md", "text/markdown; charset=utf-8"},
    {"mid", "audio/midi"},
    {"midi", "audio/midi"},
    {"mjs", "text/javascript; charset=utf-8"},
    {"mkv", "video/matroska"},
    {"mobi", "application/x-mobipocket-ebook"},
    {"mov", "video/quicktime"},
    {"mp2", "audio/mpeg"},
    {"mp3", "audio/mpeg"},
    {"mp4", "video/mp4"},
    {"mpd", "application/dash+xml"},
    {"mpeg", "video/mpeg"},
    {"mpg", "video/mpeg"},
    {"odp", "application/vnd.oasis.opendocument.presentation"},
    {"ods", "application/vnd.oasis.opendocument.spreadsheet"},
    {"odt", "application/vnd.oasis.opendocument.text"},
    {"oga", "audio/ogg"},
    {"ogg", "audio/ogg"},
    {"ogv", "video/ogg"},
    {"ogx", "application/ogg"},
    {"opus", "audio/ogg"},
    {"otf", "font/otf"},
    {"pdf", "application/pdf"},
    {"png", "image/png"},
    {"ppt", "application/vnd.ms-powerpoint"},
    {"pptx", "application/"
             "vnd.openxmlformats-officedocument.presentationml.presentation"},
    {"rar", "application/vnd.rar"},
    {"rpm", "application/x-rpm"},
    {"rss", "application/rss+xml"},
    {"rtf", "application/rtf"},
    {"sh", "text/plain; charset=utf-8"},
    {"srt", "application/x-subrip"},
    {"svg", "image/svg+xml"},
    {"tar", "application/x-tar"},
    {"text", "text/plain; charset=utf-8"},
    {"tgz", "application/gzip"},
    {"tif", "image/tiff"},
    {"tiff", "image/tiff"},
    {"toml", "application/toml"},
    {"ts", "video/mp2t"},
    {"ttf", "font/ttf"},
    {"txt", "text/plain; charset=utf-8"},
    {"vcf", "text/vcard; charset=utf-8"},
    {"vtt", "text/vtt; charset=utf-8"},
    {"wasm", "application/wasm"},
    {"wav", "audio/wav"},
    {"weba", "audio/webm"},
    {"webm", "video/webm"},
    {"webmanifest", "application/manifest+json"},
    {"webp", "image/webp"},
    {"woff", "font/woff"},
    {"woff2", "font/woff2"},
    {"xhtml", "application/xhtml+xml"},
    {"xls", "application/vnd.ms-excel"},
    {"xlsx",
     "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
    {"xml", "application/xml"},
    {"xsl", "application/xml"},
    {"xslt", "application/xslt+xml"},
    {"xz", "application/x-xz"},
    {"yaml", "application/yaml"},
    {"yml", "application/yaml"},
    {"zip", "application/zip"},
    {"zst", "application/zstd"}};

static const char *extension(const char *path)
{
    const char *base = path, *dot = NULL, *p;
    if (path == NULL)
        return NULL;
    for (p = path; *p != 0; ++p) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
            dot = NULL;
        } else if (*p == '.')
            dot = p;
    }
    return dot != NULL && dot != base && dot[1] != 0 ? dot + 1 : NULL;
}

static const char *lookup(const knurl_mime_type *types, size_t count,
                          const char *path)
{
    const char *ext = extension(path);
    size_t i;
    if (ext != NULL)
        for (i = 0; i < count; ++i)
            if (knurl_text_equal_case(ext, types[i].extension))
                return types[i].content_type;
    return NULL;
}

const char *knurl_static_content_type(const char *path)
{
    const char *type =
        lookup(defaults, sizeof(defaults) / sizeof(defaults[0]), path);
    return type != NULL ? type : "application/octet-stream";
}

const char *knurl_server_content_type(const knurl_server *server,
                                      const char *path)
{
    const char *type = server != NULL
                           ? lookup(server->config.http.mime_types,
                                    server->config.http.mime_type_count, path)
                           : NULL;
    return type != NULL ? type : knurl_static_content_type(path);
}

const char *knurl_mime_resolve(const knurl_server *server,
                               const knurl_static_config *route,
                               const char *path, const char *provided)
{
    const char *type = route->content_type;
    if (type == NULL)
        type = lookup(route->mime_types, route->mime_type_count, path);
    if (type == NULL && server != NULL)
        type = lookup(server->config.http.mime_types,
                      server->config.http.mime_type_count, path);
    if (type == NULL && provided != NULL && provided[0] != 0)
        type = provided;
    return type != NULL ? type : knurl_static_content_type(path);
}

void knurl_mime_free(const knurl_mime_type *types, size_t count)
{
    size_t i;
    if (types != NULL) {
        for (i = 0; i < count; ++i) {
            free((void *)types[i].extension);
            free((void *)types[i].content_type);
        }
        free((void *)types);
    }
}

static int extension_valid(const char *ext)
{
    const unsigned char *p = (const unsigned char *)ext;
    if (p == NULL)
        return 0;
    if (*p == '.')
        ++p;
    if (*p == 0)
        return 0;
    for (; *p != 0; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '+' || *p == '-' || *p == '_'))
            return 0;
    return 1;
}

knurl_status knurl_mime_copy(const knurl_mime_type *input, size_t count,
                             const knurl_mime_type **out, knurl_error *error)
{
    knurl_mime_type *copy;
    char *ext;
    size_t i, j;
    *out = NULL;
    if (count == 0)
        return KNURL_OK;
    if (input == NULL || count > (size_t)-1 / sizeof(*copy))
        return knurl_fail(error, KNURL_INVALID, "mime_types",
                          "provide an extension/content-type array and count");
    for (i = 0; i < count; ++i) {
        if (!extension_valid(input[i].extension) ||
            !knurl_content_type_valid(input[i].content_type))
            return knurl_fail(error, KNURL_INVALID, "mime_types",
                              "MIME entries need an extension and valid RFC "
                              "9110 Content-Type, including parameters");
        for (j = 0; j < i; ++j)
            if (knurl_text_equal_case(
                    input[i].extension + (input[i].extension[0] == '.'),
                    input[j].extension + (input[j].extension[0] == '.')))
                return knurl_fail(error, KNURL_INVALID, "mime_types",
                                  "duplicate case-insensitive MIME extension");
    }
    copy = calloc(count, sizeof(*copy));
    if (copy == NULL)
        goto no_memory;
    for (i = 0; i < count; ++i) {
        ext = knurl_copy_text(input[i].extension +
                              (input[i].extension[0] == '.'));
        copy[i].extension = ext;
        copy[i].content_type = knurl_copy_text(input[i].content_type);
        if (ext == NULL || copy[i].content_type == NULL) {
            knurl_mime_free(copy, count);
            goto no_memory;
        }
        for (; *ext != 0; ++ext)
            if (*ext >= 'A' && *ext <= 'Z')
                *ext = (char)(*ext + 'a' - 'A');
    }
    *out = copy;
    return KNURL_OK;
no_memory:
    return knurl_fail(error, KNURL_NOMEM, "mime_types",
                      "cannot copy MIME extension mappings");
}
