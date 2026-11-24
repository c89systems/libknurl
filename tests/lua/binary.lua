local work, port, mode = ...
local knurl = require('knurl')

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

local calls = 40
local database
local saved_request
record('parent start')
local server <close> = check(knurl.server{
  port = assert(tonumber(port)), workers = 1, backend_quiet = true,
  application_max_tasks = 1,
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
local routes = check(server:group{prefix = '/app'})
local invalid = routes:route{
  path = '/invalid', max_body_bytes = 20 * 1024 * 1024,
  handler = function() end,
}
assert(invalid == nil)
check(routes:route{
  path = '/:user/:action',
  methods = {'GET', 'POST'},
  buffer_bytes = 2,
  handler = function(req)
    assert(database:write(req.params.user))
    assert(database:flush())
    local user, action = req.params.user, req.params.action
    if action == 'hold' then
      record('handler waiting')
      local gate <close> = assert(io.open(work .. '/gate', 'r'))
      assert(gate:read(1) == 'x')
    elseif action == 'crash' then
      os.exit(1, false)
    elseif action == 'error' then
      error('intentional handler error')
    elseif action == 'json' then
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
