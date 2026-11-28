#include <knurl/lua_source.h>

#include "curl_core_source.inc"
#include "curl_source.inc"
#include "smtp_source.inc"

static const knurl_lua_source sources[] = {
    {"curl/core.lua",
     {(const char *)curl_core_source, sizeof(curl_core_source)}},
    {"curl/init.lua", {(const char *)curl_source, sizeof(curl_source)}},
    {"knurl/smtp.lua", {(const char *)smtp_source, sizeof(smtp_source)}}};

size_t knurl_lua_source_count(void)
{
    return sizeof(sources) / sizeof(*sources);
}

const knurl_lua_source *knurl_lua_source_at(size_t index)
{
    return index < knurl_lua_source_count() ? &sources[index] : NULL;
}
