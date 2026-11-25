local work, port = ...
local knurl = require('knurl')

local function check(value, err)
  if not value then error(err.message, 2) end
  return value
end

local login <close> = check(knurl.auth{name = 'browser', store = work .. '/state'})
local otp <close> = check(knurl.auth{name = 'otp', store = work .. '/state', login = 'totp'})
local machine <close> = check(knurl.auth{name = 'machine', store = work .. '/state', method = 'machine'})
local other <close> = check(knurl.auth{name = 'machine-other', store = work .. '/state', method = 'machine'})
local service <close> = check(knurl.auth{
  name = 'service', method = 'machine',
  client_id = 'pipeline', client_secret = 'a local test secret with plenty of characters',
})
local server <close> = check(knurl.server{
  port = assert(tonumber(port)), autoblock_disabled = true, backend_quiet = true,
  tls = {
    host = 'localhost', certificate = work .. '/cert.pem', private_key = work .. '/key.pem',
  },
})
check(server:text{path = '/health', body = 'ready'})
check(server:text{path = '/service', auth = service, body = 'allowed'})
check(server:text{path = '/machine', auth = machine, body = 'allowed'})
check(server:text{path = '/machine-other', auth = other, body = 'allowed'})
check(server:login{path = '/login', auth = login})
check(server:login{path = '/otp', auth = otp})
check(server:logout{path = '/logout', auth = login})

local app = check(server:group{prefix = '/app', auth = login})
check(app:route{
  path = '/:screen',
  handler = function(req)
    assert(req.auth.method == 'session')
    return {body = req.auth.subject .. ':' .. req.params.screen}
  end,
})
assert(not server:text{path = '/invalid', auth = function() end, body = 'no'})
check(server:run())
