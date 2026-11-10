local knurl=require('knurl')
local function ok(value,err) assert(value,err and (err.message..' '..err.detail));return value end
-- Vectis uses Lua's checked integer conversion, including exact floats/text.
for _,limit in ipairs{32.0,'32','32.0',' 32 '} do
  local owner=ok(knurl.server{port=0.0,workers={count='1'},
    limits={max_connections=limit,default_body_bytes=16.0,hard_body_bytes='32'},
    timeouts={request_header='5000.0'},supervision={control_fd=-1.0}})
  local effective=ok(owner:effective_config())
  assert(effective.workers.count==1 and effective.limits.max_connections==32)
  assert(effective.limits.default_body_bytes==16 and effective.limits.hard_body_bytes==32)
  ok(owner:route{path='/integer',method=1.0,handler=function() return 'ok' end})
  ok(owner:close())
end
for _,bad in ipairs{-1,-1.0,0.5,'0.5','-1','9223372036854775808',math.huge,-math.huge,0/0,{},true} do
  local owner,why=knurl.server{port=bad}
  assert(owner==nil and why.status==knurl.status.INVALID)
  owner,why=knurl.server{port=0,limits={default_body_bytes=bad}}
  assert(owner==nil and why.status==knurl.status.INVALID)
end
local pem=fixture.tls_setup(fixture.root..'/fixture-key.pem')
local opts={
  profile='production_webserver',start_paused=true,
  workers={count=1,affinity_disabled=true,death_policy='terminate',shutdown_timeout_ms=2000},
  listeners={
    {port=0},
    {kind='https',port=0,identity_index=1,presented_port=9443},
    {kind='redirect',port=0,identity_index=1,redirect_https_port=9443},
    {kind='https',port=0,identity_index=2},
  },
  tls={version='1.3',max_material_bytes=32768,identities={
    {domains={'localhost'},certificate={kind='memory',bytes=pem.certificate},
     private_key={kind='file',path=pem.private_key_path},client_ca={bytes=pem.ca},require_client_certificate=true},
    {domains={'bundle.local'},bundle={bytes=pem.certificate..pem.private_key}},
  }},
  timeouts={request_header=1234,keepalive={disabled=true},response_write_idle={milliseconds=5000}},
  client_ip={trusted_proxies={'127.0.0.1'}},proxy={active_exchanges=3,idle_connections=2},
  http={backend_quiet=true,server_header='lua-fixture',hsts_max_age_seconds=60,
    mime_types={['.MP3']='audio/x-fixture',custom='application/x-fixture',
      quoted='application/x-fixture; name="a;b,c"; charset="utf-8"'}},
  autoblock={max_entries=16,event_rules={{name='policy.test',threshold=2}}},
}
-- Constructor allocation failures own no sockets, TLS inputs or orphan arrays.
for n=0,35 do
  local success,value=fixture.fail_call(n,function() return knurl.server(opts) end)
  if success and value then ok(value:close()) end
end
collectgarbage('collect')
local server
local denied,allowed=0,0
opts.on_parent_ready=function(s)
  assert(s:state()=='ready_paused')
  ok(s:set_serving(true))
  ok(fixture.spawn_policy(ok(s:listener(1)).port,ok(s:listener(2)).port,ok(s:listener(3)).port))
end
opts.on_worker_exit=function() assert(allowed==2 and denied==0) end
server=ok(knurl.server(opts))
opts.http.mime_types['.MP3']='audio/x-mutated'
collectgarbage('collect')
assert(server:state()=='declared')
local config=ok(server:effective_config())
assert(config.workers.death_policy=='terminate' and config.requested_worker_count==1)
assert(config.proxy.active_exchanges==3 and config.http.server_header=='lua-fixture')
assert(config.http.mime_types.mp3=='audio/x-fixture' and config.http.mime_types.custom=='application/x-fixture')
assert(server:content_type('music/song.Mp3')=='audio/x-fixture')
assert(server:content_type('movie.mp4')=='video/mp4')
assert(server:content_type('file.custom')=='application/x-fixture')
assert(server:content_type('file.quoted')=='application/x-fixture; name="a;b,c"; charset="utf-8"')
assert(server:content_type('unknown')=='application/octet-stream')
for _,path in ipairs{false,123,'file\0.mp3'} do
  local value,why=server:content_type(path)
  assert(value==nil and why.status==knurl.status.INVALID)
end
assert(config.timeouts.request_header.milliseconds>=1234)
assert(config.timeouts.deferred_response.disabled)
assert(config.requested_timeouts.request_header.milliseconds==1234 and config.timeouts.keepalive.disabled)
assert(config.tls.version=='1.3' and config.tls.identities[1].private_key.path==pem.private_key_path)
assert(config.tls.identities[1].require_client_certificate and config.tls.identities[2].bundle.bytes==pem.certificate..pem.private_key)
assert(config.client_ip.trusted_proxies[1]=='127.0.0.1' and config.autoblock.enabled)
assert(config.autoblock.event_rules[1].name=='policy.test' and #config.autoblock.status_rules>0)
-- Core values are copied; mutation of the input or a returned snapshot is inert.
opts.client_ip.trusted_proxies[1]='192.0.2.1'
opts.autoblock.event_rules[1].name='changed'
opts.tls.identities[1].certificate.bytes='invalid'
config.tls.identities[1].private_key.path='changed'
assert(ok(server:effective_config()).tls.identities[1].private_key.path==pem.private_key_path)
ok(server:validate())
local function decision(request)
  assert(request.authorization=='Bearer fixture-token' and request.allowed_modes==3)
  assert(request.purpose==(request.resource=='/guard/entry' and 'group' or 'route'))
  if request.resource=='/auth-error' then error('expected authentication callback failure') end
  if request.resource=='/auth-invalid' then return {action='allow',body={}} end
  if request.resource=='/principal' then return {action='allow',principal=string.rep('p',300)} end
  if request.resource=='/auth-getter' then return setmetatable({},{__index=function() error('expected auth getter failure') end}) end
  if request.resource=='/auth-oom' then fixture.fail_next();return {action='allow'} end
  if request.resource=='/required' then return {action='required',www_authenticate='Basic realm="fixture"'} end
  if request.resource=='/redirect' then return {action='redirect',location='/login'} end
  if request.resource=='/deny' then return {action='deny',body='A\0B'} end
  return {action='allow',principal='alice'}
end
local auth={purpose='route',allowed_modes=0.0,provider={kind='callback',callback=decision,allowed_modes={'1.0','bearer'}}}
for _,path in ipairs{'/allow','/required','/redirect','/deny','/auth-error','/auth-invalid','/auth-getter','/auth-oom'} do
  ok(server:route{path=path,auth=auth,
    before=function(req) assert(req.principal=='alice');req.authorized=true end,
    handler=function(req)
      assert(path=='/allow' and req.authorized and req.ip=='198.51.100.7')
      allowed=allowed+1;return 'allowed'
    end,
    after=function(req) assert(req.principal=='alice' and req.authorized) end,
  })
end
ok(server:route{path='/principal',auth=auth,handler=function(req)
  assert(#req.principal==127);return 'principal'
end})
ok(server:group{prefix='/guard',auth={purpose='group',provider=auth.provider},routes={
  {path='entry',handler=function(req) assert(req.principal=='alice');allowed=allowed+1;return 'group' end},
}})
auth.provider.callback=function() error('mutated callback') end
auth.provider.allowed_modes={'unsupported'}
ok(server:get('/tls',function(req) assert(req.tls and req.listener_index==2 and req.ip=='198.51.100.7');return 'tls' end))
ok(server:get('/event',function(req) ok(req.event('policy.test'));return 'event' end))
ok(server:get('/blocked',function() denied=denied+1;error('autoblock bypass') end))
ok(server:get('/stop',function() ok(server:stop());return 'stopped' end))
ok(server:run());ok(fixture.join());ok(server:close());assert(server:state()=='closed')
-- Invalid inputs fail before starting the runtime or opening any material.
for _,bad in ipairs{
  {port=0,timeouts={request_header=-1}},
  {port=0,timeouts={keepalive={disabled=1}}},
  {port=0,workers={death_policy='unknown'}},
  {listeners={{kind='https',identity_index=0}}},
  {port=0,client_ip={trusted_proxies={'not an address'}}},
  {port=0,autoblock={enabled=true,status_rules={{status=700,threshold=1}}}},
  {port=0,tls={identities={{domains={'localhost'},mode='acme'}}}},
} do
  local value,why=knurl.server(bad)
  assert(value==nil and why.status)
end
assert(host_marker=='preserved')
for _,bad in ipairs{false,123,'not-a-table',{[1]='audio/mpeg'},
  {mp3=false},{mp3={}},{['a\0b']='text/plain'},{txt='text/plain\0evil'},
  {['.']='text/plain'},{['tar.gz']='application/gzip'},
  {mp3='invalid'},{mp3='audio/mpeg\r\nInjected: yes'},
  {txt='text/plain; charset =utf-8'},{txt='text/plain; charset= utf-8'},
  {txt='text/plain; name="unterminated'},{txt='text/plain; name=bad,value'},
  {txt='text/plain; name="value"tail'},{txt='text/plain; name="bad\\\n"'},
  {mp3='audio/mpeg',['.MP3']='audio/mp4'}} do
  local value,why=knurl.server{port=0,http={mime_types=bad}}
  assert(value==nil and why.status==knurl.status.INVALID)
end
local isolated=ok(knurl.server{port=0})
assert(isolated:content_type('song.mp3')=='audio/mpeg')
ok(isolated:close())
local value,why=isolated:content_type('song.mp3')
assert(value==nil and why.status==knurl.status.STATE)
for _,deadline in ipairs{5,{milliseconds=7},{disabled=true}} do
  local owner=ok(knurl.server{port=0,timeouts={deferred_response=deadline}})
  local effective=ok(owner:effective_config()).timeouts.deferred_response
  if type(deadline)=='table' and deadline.disabled then assert(effective.disabled)
  else assert(not effective.disabled and effective.milliseconds==(type(deadline)=='table' and 7 or 5)) end
  ok(owner:close())
end
