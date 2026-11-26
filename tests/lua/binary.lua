local work, port, mode, geo_port, runtime = ...
local knurl = require('knurl')
local pslog = require('pslog')
local curl = require('curl')
local lonejson = require('lonejson')
local smtp = require('knurl.smtp')
assert(knurl.smtp == smtp)
assert(require('curl.core') == curl.core and knurl.curl == curl.core)
assert(type(curl.version()) == 'string')

local function check(value, err)
  if not value then
    error(err.message .. (err.detail ~= '' and ' (' .. err.detail .. ')' or ''), 2)
  end
  return value
end

local function record(line)
  local file <close> = assert(io.open(work .. '/events', 'a'))
  assert(file:write(line, '\n'))
end

local country_database
if mode == 'normal' then
  local asset = work .. '/asset'
  for _, destination in ipairs{asset, work .. '/asset-alias'} do
    local result = curl.perform{url = 'file://' .. asset, proxy = '', method = 'PUT',
      upload_path = asset, download_path = destination}
    assert(not result.ok and result.error:find('must refer to different files', 1, true))
    local source <close> = assert(io.open(asset, 'rb'))
    assert(source:read('a') == 'asset\n')
  end
  local endpoint = 'http://127.0.0.1:' .. geo_port
  local expected = work .. '/geo-cache/knurl/dbip-country-lite.mmdb'
  for _, path in ipairs{'/missing', '/broken', '/bad-mmdb'} do
    local value, err = knurl.country_database{url = endpoint .. path, timeout_ms = 2000}
    assert(not value and err.message)
    assert(not io.open(expected, 'rb'))
  end
  country_database = check(knurl.country_database{url = endpoint .. '/country', timeout_ms = 2000})
  assert(country_database == expected and check(knurl.country_database()) == expected)
  local acl <close> = check(knurl.acl{
    policy = 'deny', country_database = country_database, allow = {countries = {'US'}},
    max_heap_bytes = 65536,
  })
  assert(check(acl:refresh()))
  assert(acl:check('1.2.3.4') == true and acl:check('127.0.0.1') == false)
end

local calls = 40
local database
local saved_request
local log = pslog.new_json(work .. '/server.log', {min_level = 'trace'})
log:info('Lua before Kore')
record('parent start')
local server <close> = check(knurl.server{
  port = assert(tonumber(port)), workers = 1, backend_quiet = true,
  application_max_tasks = 1, logger = log,
  application_timeout_ms = mode == 'hook-timeout' and 500 or nil,
  max_connections = mode == 'hook-timeout' and 6 or nil,
  autoblock_disabled = runtime == 'emulated',
  client_ip = {
    trusted_proxies = {'127.0.0.1/32'}, forwarded_header = 'forwarded',
    real_ip_header = '', forwarded_format = 'rfc7239', max_hops = 2,
  },
})
check(server:on_worker_start(function()
  assert(database == nil and calls == 40)
  if mode == 'init-failure' then error('intentional initializer failure') end
  database = assert(io.open(work .. '/resource', 'w+'))
  record('worker start')
end))
check(server:on_worker_stop(function()
  assert(database:close())
  record('worker stop')
  if mode == 'stop-failure' then error('intentional cleanup failure') end
end))

check(server:text{path = '/health', body = 'ready'})
check(server:static_file{path = '/asset', file_path = work .. '/asset'})
check(server:webdav{path = '/dav', cache_dir = work, site_id = 'lua-dav', auth_disabled = true})
check(server:webdav{path = '/protected-dav', cache_dir = work, site_id = 'lua-protected-dav', conceal_disabled = true})

local function wait_for_gate(message)
  record(message)
  local gate <close> = assert(io.open(work .. '/gate', 'r'))
  assert(gate:read(1) == 'x')
end

check(server:route{path = '/stream', hooks = {after = function(_, res)
  assert(res.status == 200)
  return {headers = {['x-stream'] = 'ready'}}
end}, handler = function()
  local step = 0
  return {stream = function(capacity)
    assert(capacity >= 5 and capacity <= 65536)
    step = step + 1
    if step == 1 then return 'first' end
    if step == 2 then
      wait_for_gate('stream waiting')
      return 'last'
    end
  end}
end})
local producers = setmetatable({}, {__mode = 'v'})
check(server:route{path = '/sse', methods = {'GET', 'HEAD'}, handler = function()
  collectgarbage('collect')
  assert(producers.last == nil, 'previous SSE producer is still retained')
  local sent = false
  local function produce()
    record('SSE pulled')
    if sent then return end
    sent = true
    return {event = 'update', id = '1', data = 'hello\nworld', retry_ms = 500}
  end
  producers.last = produce
  return {sse = produce}
end})

local json = lonejson.new()
local schema = json.schema('Message', {json.field('text', json.string{required = true})})
check(server:route{path = '/typed/:name', method = 'POST', request_json = schema, handler = function(req)
  return {json = schema:assign(req.input, {text = req.params.name .. ':' .. req.input:get('text')})}
end})
check(server:route{path = '/typed-xml', method = 'POST',
  request_xml = {schema = schema, root_element = 'message', text_key = ''}, handler = function(req)
    local record = schema:assign(req.input, {text = req.input:get('text') .. '!'})
    return {xml = {record = record, root_element = 'message', text_key = ''}}
  end})
check(server:openapi_doc{path = '/typed/:name', method = 'POST', operation_id = 'message',
  request_schema = schema, responses = {[200] = {description = 'Echo', schema = schema}}})
local json_api = check(server:openapi{title = 'Runner API'})
assert(json_api:find('/typed/{name}', 1, true) and json_api:find('#/components/schemas/Message', 1, true))
assert(check(server:openapi{format = 'yaml'}):find('openapi:'))
assert(not server:openapi{max_bytes = 1})

local sockets = check(server:group{prefix = '/ws'})
local current
check(sockets:websocket{path = '/:name', max_message_bytes = 1024,
  hooks = {after = function(req, res)
    if req.query == 'fail=1' then error('intentional handshake error') end
    if res.status ~= 101 then return end
    return {headers = {['x-socket'] = 'ready'}}
  end},
  on_open = function(ws, req)
    assert(database and ws:request() == req and req.params.name == 'alice')
    current = ws
    check(ws:send_text('hello ' .. req.params.name))
    wait_for_gate('websocket open waiting')
  end,
  on_message = function(ws, bytes, kind)
    assert(ws == current)
    if kind == 'text' then
      assert(bytes == 'hold')
      wait_for_gate('websocket waiting')
      check(ws:send_text('done'))
    else
      check(ws:send_binary(bytes))
    end
  end,
  on_close = function(ws, outcome)
    assert(ws == current and not ws:send_text('after close'))
    assert(outcome.peer_code == 1000 and outcome.peer_reason == 'bye' and outcome.clean)
    record('websocket closed')
    current = nil
  end,
})
check(server:websocket{path = '/ws-denied', acl = {policy = 'deny'}})
check(server:text{path = '/queue-probe', body = 'unexpected',
  hooks = {before = function() record('expired route callback ran') end}})
check(server:websocket{path = '/ws-queue', on_open = function()
  record('expired websocket callback ran')
end})

if mode == 'hook-timeout' then
  local function closed(_, outcome)
    assert(outcome.opened and outcome.clean and outcome.peer_code == 1000)
    assert(outcome.peer_reason == 'bye')
    record('websocket close delivered')
    error('intentional WebSocket close failure')
  end
  check(server:websocket{path = '/ws-close',
    on_open = function(ws) check(ws:send_text('ready')) end,
    on_close = closed,
  })
  check(server:websocket{path = '/ws-close-quiet', on_close = closed})
  check(server:text{path = '/close-blocker', body = 'released', hooks = {
    before = function() wait_for_gate('close blocker waiting') end,
  }})
end

local hook_order = {}
check(server:text{path = '/before-timeout', body = 'too late', hooks = {
  before = function() wait_for_gate('before timeout waiting') end,
}})
check(server:text{path = '/after-timeout', body = 'too late', hooks = {
  after = function()
    wait_for_gate('after timeout waiting')
    return {headers = {['x-late'] = 'too late'}}
  end,
}})
local function step(name)
  hook_order[#hook_order + 1] = name
end
local hooks = check(server:group{prefix = '/hooks', hooks = {
  auth = function(req)
    hook_order = {}
    if req.params.action == 'deny' then return false end
    step('group auth')
    return true
  end,
  before = function(req)
    assert(req.read == nil)
    step('group before')
    if req.params.action == 'block' then wait_for_gate('hook waiting') end
  end,
  after = function(req, res)
    assert(res.status == 201 and req.params.action == 'block')
    step('group after')
    return {headers = {['x-group'] = table.concat(hook_order, ', ')}}
  end,
}})
check(hooks:route{path = '/:action', method = 'POST', hooks = {
  auth = function() step('route auth'); return true end,
  before = function() step('route before') end,
  after = function(_, res)
    assert(res.status == 200)
    step('route after')
    return {status = 201, headers = {['x-route'] = 'after'}}
  end,
}, handler = function(req)
  step('handler')
  local body = assert(req:read())
  assert(not req:read())
  return body
end})

local origin = 'http://127.0.0.1:' .. port
check(server:route{path = '/proxy-origin', handler = function(req)
  assert(req.query == '')
  local configured
  for _, header in ipairs(req.headers) do
    if header.name:lower() == 'x-sent' then configured = header.value end
    assert(header.name:lower() ~= 'x-remove')
  end
  assert(configured == 'lua')
  return {body = 'upstream data', headers = {['x-origin'] = 'remove'}}
end})
local gateway = check(server:group{prefix = '/gateway'})
check(gateway:proxy{path = '/:name', target = origin .. '/wrong-prefix', alternate_targets = {origin},
  preflight = function(req)
    if req.params.name == 'deny' then return {status = 429, body = 'preflight refused'} end
    assert(req.params.name == 'hold')
  end,
  rewrite = function(req)
    assert(req.query == 'before=1')
    return {path = '/proxy-origin', target = 2, query = false,
      headers = {['x-sent'] = 'lua', ['x-remove'] = false}}
  end,
  modify_response = function(req, res)
    assert(req.params.name == 'hold' and res.status == 200)
    wait_for_gate('proxy response waiting')
    return {status = 203, headers = {['x-origin'] = false, ['x-proxy'] = 'lua'}}
  end,
  on_exchange = function(req, event)
    assert(req.params.name == 'hold')
    assert(event.status == 203 and event.downstream_bytes == #'upstream data')
    assert(event.kind == 'http' and event.route == '/gateway/:name')
    record('proxy exchange callback')
    error('intentional proxy on_exchange failure')
  end,
})
check(server:proxy{path = '/gateway-failure', target = 'https://127.0.0.1:' .. port,
  connect_timeout_ms = 500, total_timeout_ms = 1000,
  on_error = function(req, err, status)
    assert(req.path == '/gateway-failure' and err.message and (status == 502 or status == 504))
    if req.query == 'throw=1' then error('intentional proxy on_error failure') end
    return {status = 503, headers = {['x-fallback'] = 'lua'}, body = 'custom gateway response'}
  end,
})
if mode == 'normal' then
  check(server:proxy{path = '/socket-proxy', target = 'http://127.0.0.1:' .. geo_port,
    hooks = {after = function(_, res)
      assert(res.status == 101)
      wait_for_gate('proxy handshake waiting')
      return {headers = {['x-handshake'] = 'lua'}}
    end},
  })
end
if country_database then
  local country = check(server:text{path = '/country', body = 'country allowed'})
  check(country:set_acl{
    policy = 'deny', country_database = country_database, allow = {countries = {'US'}},
  })
  check(server:text{path = '/country-peer', body = 'country allowed', acl = {
    policy = 'deny', country_database = country_database, allow = {countries = {'US'}},
    use_peer = true,
  }})
end
local uploaded = work .. '/curl-uploaded'
local large = work .. '/curl-large'
local large_size = 8 * 1024 * 1024 + 1
do
  local file <close> = assert(io.open(large, 'wb'))
  assert(file:seek('set', large_size - 1))
  assert(file:write('\0'))
end
check(server:json{path = '/curl-message', body = '{"message":"hello"}'})
check(server:redirect{path = '/curl-redirect', location = '/curl-message'})
check(server:upload_file{path = '/curl-upload', file_path = uploaded})
check(server:static_file{path = '/curl-uploaded', file_path = uploaded})
check(server:static_file{path = '/curl-large', file_path = large})

local function curl_smoke()
  local base = 'http://127.0.0.1:' .. port
  local function request(path, options, fn)
    options = options or {}
    options.url = base .. path
    options.proxy = ''
    options.timeout_ms = 2000
    return (fn or curl.perform)(options)
  end
  local function upload(options, fn)
    options.method = 'POST'
    local result = request('/curl-upload', options, fn)
    assert(result.ok, result.error)
    assert(result.status == 200, tostring(result.status))
  end

  local result = request('/curl-message', nil, curl.json)
  assert(result.ok and result.status == 200 and result.json.message == 'hello', result.error)
  assert(result.effective_url == base .. '/curl-message' and result.headers:find('application/json', 1, true))
  result = request('/curl-redirect')
  assert(result.ok and result.status == 302)
  result = request('/curl-redirect', {follow_redirects = true}, curl.json)
  assert(result.ok and result.status == 200 and result.json.message == 'hello', result.error)
  result = request('/curl-missing')
  assert(result.ok and result.status == 404)

  upload({body_json = {message = 'buffered'}}, curl.json)
  result = request('/curl-uploaded', nil, curl.json)
  assert(result.ok and result.json.message == 'buffered', result.error)

  local json = lonejson.new()
  local schema = json.schema('message', {json.field('message', json.string{required = true})})
  upload({request = {schema = schema, value = {message = 'uploaded'}}}, curl.stream_json)
  result = request('/curl-uploaded', {response = {schema = schema}}, curl.stream_json)
  assert(result.ok and result.streamed and result.body == '' and result.json.message == 'uploaded', result.error)
  upload({body = '{'})
  result = request('/curl-uploaded', {response = {schema = schema}}, curl.stream_json)
  assert(not result.ok and result.json_error)

  local parts = {'one', '\0two'}
  local i = 0
  upload({read = function(bytes)
    assert(bytes >= 4)
    i = i + 1
    return parts[i]
  end})
  assert(i == 3 and request('/curl-uploaded').body == 'one\0two')
  local downloaded = work .. '/curl-downloaded'
  result = request('/curl-uploaded', {download_path = downloaded})
  assert(result.ok and result.streamed and result.body == '', result.error)
  upload({upload_path = downloaded})
  assert(request('/curl-uploaded').body == 'one\0two')
  upload({multipart = {field = 'multipart value'}})
  result = request('/curl-uploaded')
  assert(result.ok and result.body:find('name="field"', 1, true) and
    result.body:find('multipart value', 1, true), result.error)

  local asset = work .. '/asset'
  local multipart = {
    {name = 'binary', path = downloaded},
    {name = 'text', file_path = asset, filename = 'fixture.txt', content_type = 'text/plain'},
  }
  upload({multipart = multipart, download_path = work .. '/curl-multipart-response'})
  result = request('/curl-uploaded')
  assert(result.ok and result.body:find('filename="curl-downloaded"', 1, true)
    and result.body:find('one\0two', 1, true)
    and result.body:find('filename="fixture.txt"', 1, true)
    and result.body:find('Content-Type: text/plain', 1, true)
    and result.body:find('asset\n', 1, true), result.error)
  for _, destination in ipairs({downloaded, work .. '/asset-alias'}) do
    result = request('/curl-upload', {method = 'POST', multipart = multipart, download_path = destination})
    assert(not result.ok and result.error:find('different files', 1, true), result.error)
  end
  do
    local binary <close> = assert(io.open(downloaded, 'rb'))
    local text <close> = assert(io.open(asset, 'rb'))
    assert(binary:read('a') == 'one\0two' and text:read('a') == 'asset\n')
  end
  for _, path in ipairs({work .. '/gate', work .. '/missing-mime-file'}) do
    local ok, err = pcall(request, '/curl-upload', {method = 'POST', multipart = {{name = 'file', path = path}}})
    assert(not ok and err:find('readable regular file', 1, true), err)
  end

  result = request('/curl-large')
  assert(not result.ok and result.error:find('8 MiB', 1, true), result.error)
  local seen, progress = 0, 0
  result = request('/curl-large', {
    write = function(chunk) seen = seen + #chunk; return true end,
    progress = function(info) assert(type(info) == 'table'); progress = progress + 1; return true end,
  })
  assert(result.ok and result.streamed and result.body == '' and seen == large_size and progress > 0, result.error)
  result = request('/curl-message', {write = function() error('curl callback marker') end})
  assert(not result.ok and result.error:find('curl callback marker', 1, true), result.error)
  result = request('/curl-message', {write = function()
    local ok, err = pcall(request, '/curl-message')
    assert(not ok and err:find('nested', 1, true), err)
    return true
  end})
  assert(result.ok, result.error)
  assert(request('/curl-message').ok)
end

local function send_mail(port, action)
  local message = 'Subject: knurl test\r\n\r\n.payload\r\n'
  local options = {
    url = 'smtp://127.0.0.1:' .. assert(tonumber(port)),
    proxy = '', timeout_ms = 2000, from = 'sender@example.test', to = 'one@example.test',
  }
  if action == 'smtp-file' then
    local path = work .. '/message.eml'
    do
      local file <close> = assert(io.open(path, 'wb'))
      assert(file:write(message))
    end
    options.body_path = path
    options.smtp = {rcpt = {'one@example.test', 'two@example.test'}}
  elseif action == 'smtp-read' then
    local start = 1
    options.read = function(size)
      if start > #message then return nil end
      local part = message:sub(start, start + math.min(size, 4) - 1)
      start = start + #part
      return part
    end
  else
    options.message = message
  end
  if action == 'smtp-timeout' then options.timeout_ms = 250 end
  local result = smtp.send(options)
  assert(options.from == 'sender@example.test' and options.to == 'one@example.test' and options.body == nil)
  if action == 'smtp-rejected' or action == 'smtp-timeout' then
    local failure = result.error
    assert(not result.ok and not result.transport_ok, 'unexpected SMTP success')
    assert(failure.source == 'curl' and failure.dependency_code == result.code and result.error_message == failure.message)
    if action == 'smtp-timeout' then
      assert(result.code == 28 and failure.kind == 'timeout', failure.message)
    else
      assert(result.status == 550 and failure.kind == 'transport', failure.message)
    end
  else
    assert(result.ok and result.transport_ok and result.status == 250, result.error_message)
  end
  return 'smtp checked'
end

local routes = check(server:group{prefix = '/app'})
check(routes:set_acl{policy = 'deny'})
check(routes:set_acl(nil))
local invalid = routes:route{
  path = '/invalid', max_body_bytes = 20 * 1024 * 1024,
  handler = function() end,
}
assert(invalid == nil)
local application = check(routes:route{
  path = '/:user/:action',
  methods = {'GET', 'POST'},
  max_body_bytes = 16,
  buffer_bytes = 2,
  handler = function(req)
    assert(database:write(req.params.user))
    assert(database:flush())
    local user, action = req.params.user, req.params.action
    if action == 'curl-smoke' then
      curl_smoke()
      return 'curl works'
    elseif action == 'curl-mime-replay' then
      local base = 'http://127.0.0.1:' .. assert(tonumber(user))
      local result = curl.perform{
        url = base .. '/first', proxy = '', timeout_ms = 2000, follow_redirects = true,
        multipart = {{name = 'file', path = work .. '/asset'}},
      }
      assert(result.ok and result.status == 200 and result.effective_url == base .. '/second', result.error)
      return 'multipart replayed'
    elseif action:sub(1, 5) == 'smtp-' then
      return send_mail(user, action)
    elseif action == 'curl-stream' or action == 'curl-timeout' then
      local chunks = {}
      local result = curl.perform{
        url = 'http://127.0.0.1:' .. assert(tonumber(user)) .. '/from-curl',
        proxy = '', timeout_ms = action == 'curl-timeout' and 250 or 2500,
        write = function(chunk)
          if #chunks == 0 then record('curl first chunk') end
          chunks[#chunks + 1] = chunk
          return true
        end,
      }
      if action == 'curl-timeout' then
        assert(not result.ok and result.code == 28, result.error)
        return 'timed out'
      end
      assert(result.ok and result.status == 200 and result.streamed and result.body == '', result.error)
      return table.concat(chunks)
    elseif action == 'hold' then
      record('handler waiting')
      local gate <close> = assert(io.open(work .. '/gate', 'r'))
      assert(gate:read(1) == 'x')
    elseif action == 'crash' then
      os.exit(1, false)
    elseif action == 'error' then
      error('intentional handler error')
    elseif action == 'reject' then
      record('rejected upload')
      return {status = 403, body = 'rejected'}
    elseif action == 'json' then
      log:info('Lua application call', 'user', user)
      return {json = {user, {count = calls}, true}}
    elseif action == 'cycle' then
      local cycle = {}
      cycle.self = cycle
      return {json = cycle}
    elseif action == 'upload' then
      local first = check(req:read(1))
      record('upload prefix')
      local rest = {}
      while true do
        local chunk, err = req:read(2)
        if not chunk then
          assert(err == nil)
          break
        end
        rest[#rest + 1] = chunk
      end
      return first .. table.concat(rest)
    end
    if saved_request then
      local value, err = saved_request:read(1)
      assert(value == nil and err.message == 'request has ended')
      assert(saved_request:cancelled())
    end
    saved_request = req
    calls = calls + 1
    return {body = user .. ':' .. calls}
  end,
})
check(application:set_acl{policy = 'deny', allow = {addresses = {'127.0.0.1'}}, use_peer = true})

local ok, err = server:run()
if mode == 'init-failure' then
  assert(ok == nil and err.message:find('intentional initializer failure', 1, true))
  record('init failure reported')
elseif mode == 'stop-failure' then
  assert(ok == nil and err.message:find('intentional cleanup failure', 1, true))
  record('stop failure reported')
else
  check(ok, err)
end
assert(database == nil and calls == 40 and saved_request == nil)
record('parent end')
log:info('Lua after Kore')
