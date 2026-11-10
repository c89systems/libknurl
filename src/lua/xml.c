/* Lua XML object facade lifted from Vectis; native work uses public APIs. */
#include "binding.h"
#include <unistd.h>
#define XML_DOC "knurl.xml.document"
#define XML_NODE "knurl.xml.element"
#define XML_TEMP "knurl.xml.result"
#define XML_SAX "knurl.xml.sax"
typedef struct xml_document {
    knurl_xml_document *native;
    int busy;
    pid_t process;
} xml_document;
typedef struct xml_node {
    knurl_xml_node *native;
} xml_node;
typedef struct xml_result {
    knurl_owned_bytes bytes;
    knurl_xml_xpath_result xpath;
    knurl_xml_namespace namespaces[256];
} xml_result;
typedef struct xml_sax {
    knurl_xml_sax *native;
    lua_State *L;
    int self, event;
    const knurl_xml_name *name;
    const knurl_xml_attribute *attributes;
    size_t attribute_count;
    const knurl_xml_namespace *namespaces;
    size_t namespace_count;
    const void *text;
    size_t size;
} xml_sax;
static const char *xml_string(lua_State *L, int index, size_t *size, int text)
{
    const char *value;
    luaL_checktype(L, index, LUA_TSTRING);
    value = lua_tolstring(L, index, size);
    if (text && memchr(value, 0, *size) != NULL)
        luaL_argerror(L, index, "XML name/option contains NUL");
    return value;
}
static void xml_field(lua_State *L, int index, const char *name)
{
    if (lua_isnoneornil(L, index)) {
        lua_pushnil(L);
        return;
    }
    index = lua_absindex(L, index);
    lua_pushstring(L, name);
    lua_rawget(L, index);
}
static void xml_fields(lua_State *L, int index, const char *const *fields)
{
    size_t size;
    const char *key;
    int i;
    if (lua_isnoneornil(L, index))
        return;
    luaL_checktype(L, index, LUA_TTABLE);
    index = lua_absindex(L, index);
    lua_pushnil(L);
    while (lua_next(L, index)) {
        key = xml_string(L, -2, &size, 1);
        for (i = 0; fields[i] != NULL && strcmp(fields[i], key) != 0; ++i) {
        }
        if (fields[i] == NULL)
            luaL_error(L, "unknown XML option '%s'", key);
        lua_pop(L, 1);
    }
}
static size_t xml_limit(lua_State *L, int index, const char *name,
                        size_t fallback)
{
    lua_Integer value;
    xml_field(L, index, name);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return fallback;
    }
    value = luaL_checkinteger(L, -1);
    luaL_argcheck(L, value > 0 && (lua_Unsigned)value <= 2147483647u, index,
                  "XML limit must be a positive integer <=2147483647");
    lua_pop(L, 1);
    return (size_t)value;
}
static int xml_result_gc(lua_State *L)
{
    xml_result *owner = (xml_result *)luaL_checkudata(L, 1, XML_TEMP);
    knurl_owned_bytes_cleanup(&owner->bytes);
    knurl_xml_xpath_result_cleanup(&owner->xpath);
    return 0;
}
static xml_result *xml_result_new(lua_State *L)
{
    xml_result *result = (xml_result *)lua_newuserdatauv(L, sizeof(*result), 0);
    memset(result, 0, sizeof(*result));
    luaL_setmetatable(L, XML_TEMP);
    lua_toclose(L, -1);
    return result;
}
static xml_node *xml_node_new(lua_State *L, int document)
{
    xml_node *owner;
    document = lua_absindex(L, document);
    owner = (xml_node *)lua_newuserdatauv(L, sizeof(*owner), 1);
    memset(owner, 0, sizeof(*owner));
    luaL_setmetatable(L, XML_NODE);
    lua_pushvalue(L, document);
    lua_setiuservalue(L, -2, 1);
    return owner;
}
static int xml_close(lua_State *L)
{
    xml_document *document = (xml_document *)luaL_testudata(L, 1, XML_DOC);
    xml_node *node = (xml_node *)luaL_testudata(L, 1, XML_NODE);
    knurl_error error;
    knurl_status status;
    int finalizer = lua_toboolean(L, lua_upvalueindex(1));
    if (node != NULL && node->native == NULL) {
        if (!finalizer)
            lua_pushboolean(L, 1);
        return !finalizer;
    }
    if (node != NULL) {
        lua_getiuservalue(L, 1, 1);
        document = (xml_document *)luaL_checkudata(L, -1, XML_DOC);
        lua_pop(L, 1);
    }
    luaL_argcheck(L, document != NULL, 1, "XML document/element expected");
    /* A view finalizer only releases its own public handle. It cannot mutate
     * the DOM, and the active document/receiver still pins its native owner. */
    if ((document->busy && (node == NULL || !finalizer)) ||
        document->process != getpid()) {
        if (finalizer)
            return 0;
        return luaL_error(L,
                          "cannot close active or foreign-process XML object");
    }
    status = node == NULL ? knurl_xml_document_close(document->native, &error)
                          : knurl_xml_node_close(node->native, &error);
    if (status != KNURL_OK) {
        if (finalizer)
            return 0;
        return rl_error(L, &error);
    }
    if (node == NULL)
        document->native = NULL;
    else {
        node->native = NULL;
        lua_pushnil(L);
        lua_setiuservalue(L, 1, 1);
    }
    if (!finalizer)
        lua_pushboolean(L, 1);
    return !finalizer;
}
static int xml_document_new(lua_State *L)
{
    static const char *const fields[] = {"max_bytes", "max_xpath_nodes", NULL};
    knurl_xml_document_config config;
    xml_document *owner;
    knurl_error error;
    const char *data;
    size_t size;
    int result;
    lua_settop(L, 2);
    luaL_checkstack(L, 12, "XML document constructor");
    data = xml_string(L, 1, &size, 0);
    xml_fields(L, 2, fields);
    knurl_xml_document_config_init(&config);
    config.max_bytes = xml_limit(L, 2, "max_bytes", config.max_bytes);
    config.max_xpath_nodes =
        xml_limit(L, 2, "max_xpath_nodes", config.max_xpath_nodes);
    owner = (xml_document *)lua_newuserdatauv(L, sizeof(*owner), 0);
    memset(owner, 0, sizeof(*owner));
    owner->process = getpid();
    luaL_setmetatable(L, XML_DOC);
    result = lua_gettop(L);
    if (knurl_xml_document_parse(data, size, &config, &owner->native, &error) !=
        KNURL_OK)
        return rl_error(L, &error);
    lua_pushvalue(L, result);
    return 1;
}
static xml_document *xml_doc(lua_State *L)
{
    xml_document *owner = (xml_document *)luaL_checkudata(L, 1, XML_DOC);
    luaL_argcheck(L, owner->native != NULL, 1, "XML document is closed");
    return owner;
}
static xml_node *xml_node_check(lua_State *L)
{
    xml_node *owner = (xml_node *)luaL_checkudata(L, 1, XML_NODE);
    luaL_argcheck(L, owner->native != NULL, 1, "XML element view is closed");
    return owner;
}
static int xml_root(lua_State *L)
{
    xml_document *owner = xml_doc(L);
    xml_node *node = xml_node_new(L, 1);
    knurl_error error;
    if (knurl_xml_document_root(owner->native, &node->native, &error) !=
        KNURL_OK)
        return rl_error(L, &error);
    return 1;
}
static int xml_xpath(lua_State *L)
{
    xml_document *owner = xml_doc(L);
    xml_result *output;
    xml_node *node;
    knurl_error error;
    const char *expression;
    size_t size, count = 0u, i;
    int index;
    lua_settop(L, 3);
    expression = xml_string(L, 2, &size, 1);
    output = xml_result_new(L);
    if (!lua_isnil(L, 3)) {
        luaL_checktype(L, 3, LUA_TTABLE);
        lua_pushnil(L);
        while (lua_next(L, 3)) {
            luaL_argcheck(L, count < 256u, 3, "at most 256 namespace bindings");
            output->namespaces[count].prefix = xml_string(L, -2, &size, 1);
            output->namespaces[count].uri = xml_string(L, -1, &size, 1);
            ++count;
            lua_pop(L, 1);
        }
    }
    if (knurl_xml_document_xpath(owner->native, expression, output->namespaces,
                                 count, &output->xpath, &error) != KNURL_OK)
        return rl_error(L, &error);
    if (output->xpath.kind == KNURL_XML_XPATH_STRING)
        lua_pushlstring(L, (const char *)output->xpath.string.data,
                        output->xpath.string.length);
    else if (output->xpath.kind == KNURL_XML_XPATH_BOOLEAN)
        lua_pushboolean(L, output->xpath.boolean);
    else if (output->xpath.kind == KNURL_XML_XPATH_NUMBER)
        lua_pushnumber(L, output->xpath.number);
    else {
        lua_createtable(L, (int)output->xpath.count, 0);
        index = lua_gettop(L);
        for (i = 0u; i < output->xpath.count; ++i) {
            node = xml_node_new(L, 1);
            node->native = output->xpath.nodes[i];
            output->xpath.nodes[i] = NULL;
            lua_rawseti(L, index, (lua_Integer)i + 1);
        }
    }
    return 1;
}
static int xml_serialize(lua_State *L)
{
    xml_document *owner = xml_doc(L);
    xml_result *output;
    knurl_error error;
    int formatted = 0;
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TBOOLEAN);
        formatted = lua_toboolean(L, 2);
    }
    output = xml_result_new(L);
    if (knurl_xml_document_serialize_materialized(
            owner->native, formatted, &output->bytes, &error) != KNURL_OK)
        return rl_error(L, &error);
    lua_pushlstring(L, (const char *)output->bytes.data, output->bytes.length);
    return 1;
}
static int xml_name(lua_State *L)
{
    xml_node *node = xml_node_check(L);
    knurl_xml_name name;
    knurl_error error;
    if (knurl_xml_node_name(node->native, &name, &error) != KNURL_OK)
        return rl_error(L, &error);
    lua_pushstring(L, name.name);
    lua_pushstring(L, name.namespace_uri);
    lua_pushstring(L, name.prefix);
    return 3;
}
static int xml_text(lua_State *L)
{
    xml_node *node = xml_node_check(L);
    xml_result *output = xml_result_new(L);
    knurl_error error;
    if (knurl_xml_node_text(node->native, &output->bytes, &error) != KNURL_OK)
        return rl_error(L, &error);
    lua_pushlstring(L, (const char *)output->bytes.data, output->bytes.length);
    return 1;
}
static int xml_attribute(lua_State *L)
{
    xml_node *node = xml_node_check(L);
    xml_result *output;
    const char *name, *uri = NULL;
    size_t size;
    int found = 0;
    knurl_error error;
    lua_settop(L, 3);
    name = xml_string(L, 2, &size, 1);
    if (!lua_isnil(L, 3))
        uri = xml_string(L, 3, &size, 1);
    output = xml_result_new(L);
    if (knurl_xml_node_attribute(node->native, name, uri, &output->bytes,
                                 &found, &error) != KNURL_OK)
        return rl_error(L, &error);
    if (found)
        lua_pushlstring(L, (const char *)output->bytes.data,
                        output->bytes.length);
    else
        lua_pushnil(L);
    return 1;
}
static int xml_set_text(lua_State *L)
{
    xml_node *node = xml_node_check(L);
    const char *text;
    size_t size;
    knurl_error error;
    text = xml_string(L, 2, &size, 0);
    if (knurl_xml_node_set_text(node->native, text, size, &error) != KNURL_OK)
        return rl_error(L, &error);
    lua_pushboolean(L, 1);
    return 1;
}
static int xml_edit(lua_State *L)
{
    static const char *const attr_fields[] = {"namespace", "prefix", NULL};
    static const char *const child_fields[] = {"namespace", "prefix", "text",
                                               NULL};
    int child = lua_toboolean(L, lua_upvalueindex(1)), options = child ? 3 : 4;
    xml_node *node = xml_node_check(L), *output = NULL;
    knurl_xml_child_config config;
    knurl_error error;
    knurl_status status;
    const char *name, *value = NULL;
    size_t size, value_size = 0u;
    lua_settop(L, options);
    memset(&config, 0, sizeof(config));
    name = xml_string(L, 2, &size, 1);
    if (!child)
        value = xml_string(L, 3, &value_size, 0);
    xml_fields(L, options, child ? child_fields : attr_fields);
    xml_field(L, options, "namespace");
    if (!lua_isnil(L, -1))
        config.namespace_uri = xml_string(L, -1, &size, 1);
    xml_field(L, options, "prefix");
    if (!lua_isnil(L, -1))
        config.prefix = xml_string(L, -1, &size, 1);
    if (child) {
        xml_field(L, options, "text");
        if (!lua_isnil(L, -1)) {
            config.text_present = 1;
            config.text = xml_string(L, -1, &config.text_size, 0);
        }
        lua_getiuservalue(L, 1, 1);
        output = xml_node_new(L, -1);
        status = knurl_xml_node_append_child(node->native, name, &config,
                                             &output->native, &error);
    } else
        status = knurl_xml_node_set_attribute(node->native, name, value,
                                              value_size, config.namespace_uri,
                                              config.prefix, &error);
    if (status != KNURL_OK)
        return rl_error(L, &error);
    if (!child)
        lua_pushboolean(L, 1);
    return 1;
}
static int xml_remove(lua_State *L)
{
    xml_node *node = xml_node_check(L);
    knurl_error error;
    if (knurl_xml_node_remove(node->native, &error) != KNURL_OK)
        return rl_error(L, &error);
    lua_pushboolean(L, 1);
    return 1;
}
static int xml_guard(lua_State *L)
{
    xml_document *document;
    int count = lua_gettop(L), status;
    luaL_checkstack(L, 24, "XML method stack");
    document = (xml_document *)luaL_testudata(L, 1, XML_DOC);
    if (document == NULL) {
        (void)xml_node_check(L);
        lua_getiuservalue(L, 1, 1);
        document = (xml_document *)luaL_checkudata(L, -1, XML_DOC);
        lua_pop(L, 1);
    }
    luaL_argcheck(L, !document->busy && document->process == getpid(), 1,
                  "cannot reenter active or foreign-process XML document");
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    document->busy = 1;
    status = lua_pcall(L, count, LUA_MULTRET, 0);
    document->busy = 0;
    if (status != LUA_OK)
        return lua_error(L);
    return lua_gettop(L);
}
static void xml_name_fields(lua_State *L, const knurl_xml_name *name)
{
    lua_pushstring(L, name->namespace_uri);
    lua_setfield(L, -2, "uri");
    lua_pushstring(L, name->prefix);
    lua_setfield(L, -2, "prefix");
}
static int xml_sax_dispatch(lua_State *L)
{
    static const char *const names[] = {"start_element", "end_element", "text"};
    xml_sax *owner = (xml_sax *)lua_touserdata(L, 1);
    size_t i;
    int attrs, namespaces;
    lua_getiuservalue(L, 2, 1);
    xml_field(L, -1, names[owner->event]);
    if (lua_isnil(L, -1))
        return 0;
    if (owner->event == 2) {
        lua_pushlstring(L, (const char *)owner->text, owner->size);
        lua_call(L, 1, 2);
    } else {
        lua_pushstring(L, owner->name->name);
        if (owner->event == 0) {
            lua_createtable(L, (int)owner->attribute_count, 0);
            attrs = lua_gettop(L);
            for (i = 0u; i < owner->attribute_count; ++i) {
                lua_createtable(L, 0, 4);
                lua_pushstring(L, owner->attributes[i].name.name);
                lua_setfield(L, -2, "name");
                lua_pushstring(L, owner->attributes[i].name.namespace_uri);
                lua_setfield(L, -2, "namespace");
                lua_pushstring(L, owner->attributes[i].name.prefix);
                lua_setfield(L, -2, "prefix");
                lua_pushlstring(L, (const char *)owner->attributes[i].value,
                                owner->attributes[i].length);
                lua_setfield(L, -2, "value");
                lua_rawseti(L, attrs, (lua_Integer)i + 1);
            }
        }
        lua_createtable(L, 0, 3);
        xml_name_fields(L, owner->name);
        if (owner->event == 0) {
            lua_createtable(L, (int)owner->namespace_count, 0);
            namespaces = lua_gettop(L);
            for (i = 0u; i < owner->namespace_count; ++i) {
                lua_createtable(L, 0, 2);
                lua_pushstring(L, owner->namespaces[i].prefix);
                lua_setfield(L, -2, "prefix");
                lua_pushstring(L, owner->namespaces[i].uri);
                lua_setfield(L, -2, "uri");
                lua_rawseti(L, namespaces, (lua_Integer)i + 1);
            }
            lua_setfield(L, -2, "declarations");
        }
        lua_call(L, owner->event == 0 ? 3 : 2, 2);
    }
    if (lua_isnil(L, -2) && !lua_isnil(L, -1))
        return lua_error(L);
    return 0;
}
static knurl_status xml_sax_call(xml_sax *owner, knurl_error *error)
{
    lua_State *L = owner->L;
    int base = lua_gettop(L), status;
    if (!lua_checkstack(L, 6)) {
        return rl_fail(error, KNURL_NOMEM,
                       "SAX callback stack allocation failed");
    }
    lua_pushcfunction(L, xml_sax_dispatch);
    lua_pushlightuserdata(L, owner);
    lua_pushvalue(L, owner->self);
    status = lua_pcall(L, 2, 0, 0);
    if (status != LUA_OK) {
        lua_pushvalue(L, -1);
        lua_setiuservalue(L, owner->self, 2);
        rl_fail(error, status == LUA_ERRMEM ? KNURL_NOMEM : KNURL_STATE,
                "SAX callback failed");
    }
    lua_settop(L, base);
    return status == LUA_OK ? KNURL_OK : error->status;
}
static knurl_status xml_sax_start(void *context, const knurl_xml_name *name,
                                  const knurl_xml_attribute *attrs,
                                  size_t count,
                                  const knurl_xml_namespace *namespaces,
                                  size_t namespace_count, knurl_error *error)
{
    xml_sax *owner = (xml_sax *)context;
    owner->event = 0;
    owner->name = name;
    owner->attributes = attrs;
    owner->attribute_count = count;
    owner->namespaces = namespaces;
    owner->namespace_count = namespace_count;
    return xml_sax_call(owner, error);
}
static knurl_status xml_sax_end(void *context, const knurl_xml_name *name,
                                knurl_error *error)
{
    xml_sax *owner = (xml_sax *)context;
    owner->event = 1;
    owner->name = name;
    return xml_sax_call(owner, error);
}
static knurl_status xml_sax_text(void *context, const void *data, size_t size,
                                 knurl_error *error)
{
    xml_sax *owner = (xml_sax *)context;
    owner->event = 2;
    owner->text = data;
    owner->size = size;
    return xml_sax_call(owner, error);
}
static int xml_sax_close(lua_State *L)
{
    xml_sax *owner = (xml_sax *)luaL_checkudata(L, 1, XML_SAX);
    int finalizer = lua_toboolean(L, lua_upvalueindex(1));
    knurl_error error;
    if (knurl_xml_sax_close(owner->native, &error) != KNURL_OK) {
        if (finalizer)
            return 0;
        return rl_error(L, &error);
    }
    owner->native = NULL;
    lua_pushnil(L);
    lua_setiuservalue(L, 1, 1);
    lua_pushnil(L);
    lua_setiuservalue(L, 1, 2);
    if (!finalizer)
        lua_pushboolean(L, 1);
    return !finalizer;
}
static int xml_sax_new(lua_State *L)
{
    static const char *const callbacks[] = {"start_element", "end_element",
                                            "text", NULL};
    static const char *const fields[] = {"max_metadata_bytes", NULL};
    knurl_xml_sax_callbacks native_callbacks;
    xml_sax *owner;
    knurl_error error;
    size_t limit;
    int i, result, copied;
    lua_settop(L, 2);
    luaL_checkstack(L, 12, "SAX constructor stack");
    luaL_checktype(L, 1, LUA_TTABLE);
    xml_fields(L, 1, callbacks);
    xml_fields(L, 2, fields);
    limit = xml_limit(L, 2, "max_metadata_bytes", KNURL_XML_DEFAULT_MAX_BYTES);
    owner = (xml_sax *)lua_newuserdatauv(L, sizeof(*owner), 2);
    memset(owner, 0, sizeof(*owner));
    luaL_setmetatable(L, XML_SAX);
    result = lua_gettop(L);
    lua_createtable(L, 0, 3);
    copied = lua_gettop(L);
    for (i = 0; callbacks[i] != NULL; ++i) {
        xml_field(L, 1, callbacks[i]);
        if (!lua_isnil(L, -1))
            luaL_checktype(L, -1, LUA_TFUNCTION);
        lua_setfield(L, copied, callbacks[i]);
    }
    lua_setiuservalue(L, result, 1);
    memset(&native_callbacks, 0, sizeof(native_callbacks));
    native_callbacks.start_element = xml_sax_start;
    native_callbacks.end_element = xml_sax_end;
    native_callbacks.text = xml_sax_text;
    if (knurl_xml_sax_new(&native_callbacks, owner, limit, &owner->native,
                          &error) != KNURL_OK)
        return rl_error(L, &error);
    lua_pushvalue(L, result);
    return 1;
}
static int xml_sax_process(lua_State *L)
{
    xml_sax *owner = (xml_sax *)luaL_checkudata(L, 1, XML_SAX);
    int finish = lua_toboolean(L, lua_upvalueindex(1));
    knurl_error error;
    knurl_status status;
    const char *data = NULL;
    size_t size = 0u;
    luaL_checkstack(L, 12, "SAX feed stack");
    luaL_argcheck(L, owner->native != NULL, 1, "SAX receiver is closed");
    if (owner->L != NULL) {
        /* Let the public parser latch reentry before validating nested Lua
         * arguments. Preserve the outer callback's state and borrowed spans. */
        (void)knurl_xml_sax_feed(owner->native, NULL, 0, &error);
        return rl_error(L, &error);
    }
    if (!finish)
        data = xml_string(L, 2, &size, 0);
    owner->L = L;
    owner->self = 1;
    status = finish ? knurl_xml_sax_finish(owner->native, &error)
                    : knurl_xml_sax_feed(owner->native, data, size, &error);
    owner->L = NULL;
    if (status != KNURL_OK) {
        lua_getiuservalue(L, 1, 2);
        if (!lua_isnil(L, -1)) {
            lua_pushnil(L);
            lua_insert(L, -2);
            return 2;
        }
        lua_pop(L, 1);
        return rl_error(L, &error);
    }
    lua_pushboolean(L, 1);
    return 1;
}
static void xml_method(lua_State *L, const char *name, lua_CFunction fn)
{
    lua_pushcfunction(L, fn);
    lua_pushcclosure(L, xml_guard, 1);
    lua_setfield(L, -2, name);
}
static void xml_metatable(lua_State *L, const char *name, lua_CFunction close)
{
    luaL_newmetatable(L, name);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    lua_pushboolean(L, 0);
    lua_pushcclosure(L, close, 1);
    lua_setfield(L, -2, "close");
    lua_pushboolean(L, 1);
    lua_pushcclosure(L, close, 1);
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, "__gc");
    lua_setfield(L, -2, "__close");
}
void rl_xml_objects_open(lua_State *L)
{
    int module = lua_absindex(L, -1);
    xml_metatable(L, XML_DOC, xml_close);
    xml_method(L, "root", xml_root);
    xml_method(L, "xpath", xml_xpath);
    xml_method(L, "serialize_materialized", xml_serialize);
    lua_pop(L, 1);
    xml_metatable(L, XML_NODE, xml_close);
    xml_method(L, "name", xml_name);
    xml_method(L, "text", xml_text);
    xml_method(L, "attribute", xml_attribute);
    xml_method(L, "set_text", xml_set_text);
    xml_method(L, "remove", xml_remove);
    lua_pushboolean(L, 0);
    lua_pushcclosure(L, xml_edit, 1);
    lua_pushcclosure(L, xml_guard, 1);
    lua_setfield(L, -2, "set_attribute");
    lua_pushboolean(L, 1);
    lua_pushcclosure(L, xml_edit, 1);
    lua_pushcclosure(L, xml_guard, 1);
    lua_setfield(L, -2, "append_child");
    lua_pop(L, 1);
    luaL_newmetatable(L, XML_TEMP);
    lua_pushcfunction(L, xml_result_gc);
    lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, xml_result_gc);
    lua_setfield(L, -2, "__close");
    lua_pop(L, 1);
    xml_metatable(L, XML_SAX, xml_sax_close);
    lua_pushboolean(L, 0);
    lua_pushcclosure(L, xml_sax_process, 1);
    lua_setfield(L, -2, "feed");
    lua_pushboolean(L, 1);
    lua_pushcclosure(L, xml_sax_process, 1);
    lua_setfield(L, -2, "finish");
    lua_pop(L, 1);
    lua_pushcfunction(L, xml_document_new);
    lua_setfield(L, module, "document");
    lua_pushcfunction(L, xml_sax_new);
    lua_setfield(L, module, "sax");
}
