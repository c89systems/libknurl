local knurl=require('knurl')
local native=require('knurl.native')
local function ok(value,err) assert(value,err and (err.message..' '..err.detail));return value end
local function state(value,err) assert(value==nil and err.status==knurl.status.STATE) end
fixture.prepare_body()
local material=fixture.tls_setup(fixture.workspace..'/proxy.key')
local origin=fixture.proxy_setup()
for n=0,28,4 do
  local owner=ok(native.server{listeners={{port=0}}})
  fixture.fail_call(n,function() return owner:proxy{path='/allocation',target=origin.http,
    alternate_targets={origin.http},rewrite=function() end,modify_response=function() end} end)
  assert(owner:route_count()<=1);ok(owner:close())
end
collectgarbage('collect')
local server=ok(knurl.server{
  port=0,workers={count=1,affinity_disabled=true},http={backend_quiet=true},
  on_parent_ready=function(s) ok(fixture.spawn_proxy(ok(s:listener()).port)) end,
})
for _,edit in ipairs{{path='nul\0path'},{target=origin.http..'\0suffix'}, {connect_timeout_ms=-1},
  {total_timeout_ms=0.5}, {buffer_limit_bytes=1}, {upstream_http_version='bad'},
  {tls_client_key_pem=material.private_key}, {tls_ca_pem=''}, {alternate_targets={origin.http..'\0x'}},
  {rewrite=false},{modify_response=false},{before=function() end}} do
  local opts={path='/invalid',target=origin.http}
  for k,v in pairs(edit) do opts[k]=v end
  local value,err=server:proxy(opts)
  assert(value==nil and err.status==knurl.status.INVALID)
end
local pre,output,response,rewrites,modified,errors=nil,nil,nil,0,0,0
local function preflight(input)
  assert(input.body==nil and input.host=='localhost' and type(input.headers)=='table')
  if pre then state(pre:param('id')) end
  pre=input
  if input.path=='/proxy/ws' then assert(input.websocket);return {status=403,body='ws'} end
  assert(not input.websocket)
  if input.path=='/local' then
    return setmetatable({}, {__index=function(_,name)
      state(input:param('id'))
      if name=='status' then return 403 end
      if name=='body' then return 'A\0B' end
      if name=='headers' then return {{name='Set-Cookie',value='a=1'},{name='Set-Cookie',value='b=2'}} end
    end})
  end
  if input.path=='/preflight-error' then error('expected preflight failure') end
  if input.path=='/preflight-oom' then fixture.fail_next();return string.rep('x',30000) end
  if input.path=='/local-getter' then return setmetatable({status=403,body='copied'}, {__index=function() error('expected local getter failure') end}) end
  if input.path=='/local-header' then return {status=403,headers={{name='X-Test',value='bad\0value'}}} end
  if input.path=='/local-body' then return {status=403,body=string.rep('x',65537)} end
  input.path='edited-copy'
end
local function rewrite(input,out)
  rewrites=rewrites+1
  state(pre:param('id'));if output then state(output:set_path('/expired')) end
  output=out
  assert(input.path~='edited-copy' and (input.method=='GET' or input.method=='POST'))
  if input.path=='/proxy/echo' then ok(out:set_path('/echo'))
  elseif input.path=='/param/id' then
    assert(input:param('id')=='id' and input.param('absent')==nil)
    ok(out:select_target(2));ok(out:set_method('PATCH'));ok(out.set_path('/check'))
    ok(out:set_query('q=changed'));ok(out:set_host('changed.test:9'))
    ok(out:add_header('X-Added','one'));ok(out:add_header('X-Added','two'));ok(out:remove_header('X-Test'))
  elseif input.path=='/rewrite-error' then error('expected rewrite failure')
  elseif input.path=='/rewrite-oom' then fixture.fail_next();return string.rep('x',30000)
  elseif input.path=='/rewrite-invalid' then assert(out:select_target(0)==nil)
  elseif input.path=='/rewrite-nul' then assert(out:set_path('/bad\0suffix')==nil)
  elseif input.path=='/rewrite-method' then assert(out:set_method('get')==nil)
  elseif input.path=='/rewrite-type' then assert(out:add_header({},'value')==nil)
  else ok(out:set_query(nil));ok(out:set_host(nil));ok(out:set_header('X-Test','copied')) end
end
local function modify(res)
  modified=modified+1
  if output then state(output:set_path('/expired')) end
  if response then state(response:set_status(200)) end
  response=res
  assert(res.status==200 and res.body==nil and type(res.headers)=='table')
  for _,header in ipairs(res.headers) do
    if header.name:lower()=='content-type' and header.value=='text/event-stream' then
      ok(res:set_header('X-Proxy','knurl'));return
    end
  end
  res.status=599;ok(res:set_status(201))
  ok(res:add_header('X-Proxy','before'));ok(res:set_header('X-Proxy','knurl'));ok(res:remove_header('X-Remove'))
end
local function gateway(err)
  errors=errors+1
  assert(type(err.status)=='number' and type(err.code)=='number' and type(err.message)=='string')
  io.stderr:write('proxy fixture cause: '..err.message..'\n')
  return {status=503,body='gateway',headers={{name='X-Gateway',value='local'}}}
end
local exchange_ids={}
local exchange_count=0
local function exchange(e)
  assert(type(e.request_id)=='number' and not exchange_ids[e.request_id])
  exchange_ids[e.request_id]=true;exchange_count=exchange_count+1
  assert(type(e.route)=='string' and type(e.upstream)=='string' and e.kind=='http')
  assert(e.upstream_bytes>=0 and e.downstream_bytes>=0 and e.duration_ms>=0)
  assert(type(e.disconnect_side)=='string' and type(e.error_category)=='string')
  if proxy_terminal_mode=='oom' then fixture.fail_next();string.rep('x',30000) end
  if proxy_terminal_mode then error('expected terminal snapshot callback failure') end
end
local function declare(path,changes)
  local opts={path=path,target=origin.http,preflight=preflight,rewrite=rewrite,modify_response=modify,on_error=gateway,
    buffer_limit_bytes=8192,upstream_http_version='http1',on_exchange=exchange}
  for k,v in pairs(changes or {}) do opts[k]=v end
  ok(server:proxy(opts));return opts
end
local opts=declare('^/proxy/.*$',{path_kind='regex'})
opts.target=origin.unavailable
declare('/param/:id',{path_kind='params',alternate_targets={(origin.http:gsub('/base$','/alternate'))}})
opts=nil
for _,path in ipairs{'/local','/preflight-error','/preflight-oom','/local-getter','/local-header','/local-body',
  '/rewrite-error','/rewrite-oom','/rewrite-invalid','/rewrite-nul','/rewrite-method','/rewrite-type'} do declare(path) end
for _,phase in ipairs{'error','oom','invalid','nul','type'} do
  declare('/response-'..phase,{modify_response=function(res)
    response=res
    if phase=='error' then error('expected response failure') end
    if phase=='oom' then fixture.fail_next();return string.rep('x',30000) end
    if phase=='invalid' then assert(res:set_header('Content-Length','0')==nil) end
    if phase=='nul' then assert(res:set_header('X-Test','bad\0suffix')==nil) end
    if phase=='type' then assert(res:set_status({})==nil) end
  end})
end
declare('/secure',{target=origin.tls,tls_ca_pem=material.ca,tls_client_cert_pem=material.certificate,tls_client_key_pem=material.private_key})
declare('/bad-client',{target=origin.tls,tls_ca_pem=material.ca,tls_client_cert_pem='invalid certificate',tls_client_key_pem=material.private_key,
  on_error=function(err)
    assert(err.message~='Problem with the local SSL certificate' and err.message:lower():find('certificate',1,true))
    return gateway(err)
  end})
-- The failure origin resets connections; keep its connect deadline bounded.
declare('/gateway',{target=origin.unavailable,connect_timeout_ms=250})
declare('/gateway-error',{target=origin.unavailable,connect_timeout_ms=250,on_error=function() error('expected gateway failure') end})
declare('/gateway-getter',{target=origin.unavailable,connect_timeout_ms=250,on_error=function() return setmetatable({status=503,body='copied'}, {__index=function() error('expected gateway getter failure') end}) end})
ok(server:get('/stop',function()
  state(pre:param('id'));state(output:set_path('/expired'));state(response:set_status(200))
  assert(rewrites>=10 and modified>=4 and errors>=7)
  assert(exchange_count>=5)
  ok(server:stop());return 'ok'
end))
ok(server:validate())
local result,cause=server:run()
if proxy_terminal_mode then assert(result==nil and cause.status) else ok(result,cause) end
ok(fixture.join());ok(fixture.proxy_join());ok(server:close())
ok(os.remove(fixture.workspace..'/proxy.key'))
