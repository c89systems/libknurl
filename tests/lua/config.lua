local module_dir, port, upstream, fixture, uploaded = ...
package.cpath = module_dir .. '/?.so'
local knurl = require('knurl')

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
