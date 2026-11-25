local module_dir, port, upstream, fixture, uploaded = ...
package.cpath = module_dir .. '/?.so'
package.path = module_dir .. '/?/init.lua;' .. module_dir .. '/?.lua'
local knurl = require('knurl')
local pslog = require('pslog')
local curl = require('curl')
local lonejson = require('lonejson')
assert(require('curl.core') == curl.core and knurl.curl == curl.core)
assert(type(curl.version()) == 'string')
local transfer = curl.perform{url = 'file://' .. fixture}
assert(not transfer.ok and transfer.code == 1, transfer.error)
for _, options in ipairs{
  {url = 'http://local\0host'},
  {url = 'http://localhost', no_signal = false},
  {url = 'http://localhost', timeout_ms = -1},
  {url = 'http://localhost', headers = {bad = 'first\r\nsecond'}},
  {url = 'http://localhost', read = true},
  {url = 'http://localhost', body = 'value', multipart = {field = 'value'}},
} do
  assert(not pcall(curl.perform, options))
end
do
  local json = lonejson.new()
  local schema = json.schema('message', {json.field('message', json.string{required = true})})
  local result = curl.stream_json{
    url = upstream .. '/curl-module', proxy = '', timeout_ms = 2000,
    response = {schema = schema},
  }
  assert(result.ok and result.status == 200 and result.json.message == 'loaded' and result.streamed, result.error)
end

local function ok(value, err)
  if not value then error(err.message .. ' (' .. err.detail .. ')', 2) end
  return value
end

local function rejects(field, value, err)
  assert(value == nil and err.kind == 'invalid argument', tostring(err and err.kind))
  assert(err.detail == field, err.detail)
end

rejects('callbacks.on_request_start', knurl.server{
  callbacks = {on_request_start = function() error('unexpected') end},
})
local server <close> = ok(knurl.server{
  port = port, workers = 1, backend_quiet = true, callbacks = {},
})
local weak = setmetatable({}, {__mode = 'v'})
do
  local root = pslog.new_json{min_level = 'disabled'}
  weak[1] = root
  ok(server:set_pslog(root, {success_level = 'info', status_levels = {[404] = 'disabled'}}))
end
collectgarbage('collect')
assert(weak[1] ~= nil)
local unsafe = pslog.new_json(function() error('unexpected Lua sink invocation') end)
local changed, log_error = server:set_pslog(unsafe)
assert(changed == nil and log_error.message:find('native sink', 1, true))
collectgarbage('collect')
assert(weak[1] ~= nil)
rejects('hooks.before', server:group{prefix = '/bad', hooks = {before = function() end}})
rejects('auth', server:static_file{path = '/bad', file_path = fixture, auth = {}})
rejects('hooks.after', server:upload_file{path = '/bad', file_path = uploaded,
  hooks = {after = function() end}})
rejects('rewrite', server:proxy{path = '/bad', target = upstream, rewrite = function() end})
rejects('handler', server:application{path = '/bad', target = upstream, handler = function() end})
assert(not server:redirect{path = '/invalid', location = '/health\r\nX-Extra: injected'})
assert(not server:redirect{path = '/invalid', location = ''})
assert(not server:text{path = '/invalid', method = {}})

local data = {path = '/health', body = 'ready', hooks = {}}
ok(server:text(data))
data.body = 'changed'
ok(server:redirect{path = '/redirect', location = '/health', status = 307})
ok(server:json{path = '/json', body = '{"native":true}'})
ok(server:text{path = '/bytes', body = 'A\0B'})
ok(server:static_file{path = '/missing', file_path = fixture .. '.missing'})
ok(server:static_file{path = '/uploaded', file_path = uploaded})
ok(server:upload_file{path = '/upload', file_path = uploaded, max_body_bytes = 8})
ok(server:upload_file{path = '/upload-default', file_path = uploaded})
ok(server:proxy{path = '/proxy', target = upstream})
ok(server:application{path = '/app', target = upstream, total_timeout_ms = 10000})
ok(server:application{path = '/timeout', target = upstream, total_timeout_ms = 1000})

local deny = ok(knurl.acl{policy = 'deny'})
local denied = ok(server:group{prefix = '/deny', acl = deny})
ok(denied:text{path = '/text', body = 'blocked'})
ok(deny:close())
local native = ok(server:group{prefix = '/native', hooks = {}})
ok(native:static_file{path = '/static', file_path = fixture})
ok(native:text{path = '/acl', body = 'allow', acl = {
  policy = 'deny', allow = {addresses = {'127.0.0.1/32'}},
}})
ok(server:static_directory{path = '/directory', root_dir = fixture:match('^(.*)/')})
collectgarbage('collect')
ok(server:validate())
ok(server:run())
ok(server:close())
collectgarbage('collect')
assert(weak[1] == nil)
