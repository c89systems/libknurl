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
