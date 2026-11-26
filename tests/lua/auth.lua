local work, port, plain_port = ...
local knurl = require('knurl')

local function check(value, err)
  if not value then error(err.message, 2) end
  return value
end

local login <close> = check(knurl.auth{name = 'browser', store = work .. '/state'})
local otp <close> = check(knurl.auth{name = 'otp', store = work .. '/state', login = 'totp'})
local machine <close> = check(knurl.auth{name = 'machine', store = work .. '/state', method = 'machine'})
local provision <close> = check(knurl.auth{
  name = 'provision', store = work .. '/state', method = 'machine', steps = {'password'},
})
local oauth <close> = check(knurl.oauth2{
  token_endpoint = 'https://localhost:' .. port .. '/missing-token',
  ca_file = work .. '/cert.pem', client_id = 'fixture', client_secret = 'secret',
})
local other <close> = check(knurl.auth{name = 'machine-other', store = work .. '/state', method = 'machine'})
local service <close> = check(knurl.auth{
  name = 'service', method = 'machine',
  client_id = 'pipeline', client_secret = 'a local test secret with plenty of characters',
})
local server <close> = check(knurl.server{
  listeners = {
    {name = 'secure', kind = 'https', port = assert(tonumber(port)), identity = 1},
    {name = 'redirect', kind = 'redirect', port = assert(tonumber(plain_port)), redirect_https_port = tonumber(port)},
  },
  autoblock_disabled = true, backend_quiet = true,
  tls = {
    identities = {{domains = {'localhost'}, certificate = work .. '/cert.pem', private_key = work .. '/key.pem'}},
  },
})
check(server:text{path = '/health', body = 'ready'})
check(server:text{path = '/service', auth = service, body = 'allowed'})
check(server:text{path = '/machine', auth = machine, body = 'allowed'})
check(server:text{path = '/machine-other', auth = other, body = 'allowed'})
check(server:text{path = '/issued', auth = provision, body = 'allowed'})
check(server:auth_workflow{path = '/provision', auth = provision})
check(server:proxy{path = '/unavailable-token', target = 'https://localhost:' .. port, oauth2 = oauth})
check(server:proxy{
  path = '/token-fallback', target = 'https://localhost:' .. port, oauth2 = oauth,
  on_error = function(req, err, status)
    assert(req.path == '/token-fallback' and status == 502)
    assert(err.message:find('token endpoint unavailable', 1, true))
    if req.query == 'throw=1' then error('token fallback failed') end
    return {status = 503, headers = {['x-fallback'] = 'oauth2'}, body = 'token service unavailable'}
  end,
})
check(server:login{path = '/login', auth = login})
check(server:login{path = '/otp', auth = otp})
check(server:totp_enroll{path = '/totp-enroll', auth = login})
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
assert(not knurl.auth{name = 'invalid', store = work .. '/state', steps = {'totp', 'password'}})
check(server:run())
