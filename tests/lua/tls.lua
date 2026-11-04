local rotmos=require('rotmos')
local function ok(value,err) if not value then error(debug.traceback(err and err.message or 'operation failed'),2) end;return value end
local function failure(value,err,status) assert(value==nil and err.status==status,err and err.message) end
local function state(value,err) failure(value,err,rotmos.status.STATE) end
local pem=fixture.tls_setup(fixture.root..'/tls-callback-key.pem')
local key_file=assert(io.open((fixture.script:match('^(.*)/') or '.')..'/acme-key.pem','rb'))
local acme_key=assert(key_file:read('a'));key_file:close()
local function restore_keys(directory)
  fixture.private_directory(directory..'/certificates')
  fixture.private_directory(directory..'/certificates/localhost')
  for _,name in ipairs{'account-key.pem','certificates/localhost/key.pem'} do
    local file=assert(io.open(directory..'/'..name,'wb'));assert(file:write(acme_key));assert(file:close())
  end
end
local opens,closes,resets=0,0,0
local retained,server
local function source(text,legacy)
  local sent=false
  local declaration={read=function(capacity)
    assert(capacity>0)
    if not sent then sent=true;return text end
    if legacy then return nil end
    return nil,rotmos.EOF
  end,close=function(cause)
    closes=closes+1
    if not legacy then assert(cause==rotmos.status.OK) end
  end}
  if legacy then declaration.reset=function() resets=resets+1;sent=false end;sent=true;return declaration end
  retained=ok(rotmos.source(declaration));return retained
end
local declaration={domains={'localhost'},certificate={open=function()
  opens=opens+1;state(server:validate());state(server:run());state(server:close())
  collectgarbage('collect');return source(pem.certificate,true)
end},private_key={kind='callback',open=function() opens=opens+1;return source(pem.private_key,false) end},
  extra_chain={open=function() opens=opens+1;return source(pem.ca,true) end},client_ca={bytes=pem.ca},require_client_certificate=true}
local weak=setmetatable({declaration.certificate.open},{__mode='v'})
server=ok(rotmos.server{workers=1,listeners={{kind='https',identity_index=1,port=0}},tls={identities={declaration}},
  on_parent_ready=function(s) ok(fixture.spawn_single(ok(s:listener(1)).port,true)) end})
assert(opens==0 and closes==0)
declaration.certificate.open=function() error('mutated opener') end
collectgarbage('collect');assert(weak[1]~=nil)
assert(ok(server:effective_config()).tls.identities[1].certificate.kind=='callback')
ok(server:validate());assert(opens==3 and closes==3 and resets==2)
state(retained:read(1));ok(retained:close())
ok(server:get('/',function() ok(server:stop());return 'ok' end))
ok(server:run());ok(fixture.join());assert(opens==6 and closes==6 and resets==4)
ok(server:close());collectgarbage('collect');assert(weak[1]==nil)
for _,mode in ipairs{'again','empty','read-error','close-error','read-close-error','bound','open-error','returned-error','invalid','oom'} do
  local closed=0
  local selected
  local config={workers=1,listeners={{kind='https',identity_index=1,port=0}},tls={max_material_bytes=mode=='bound' and 8 or 32768,
    identities={{domains={'localhost'},private_key=mode=='bound' and {open=function() return source(pem.private_key,true) end} or {bytes=pem.private_key},certificate={open=function()
      if mode=='open-error' then error('open failure') end
      if mode=='returned-error' then return nil,{status=rotmos.status.CONFLICT,message='opener returned error'} end
      if mode=='invalid' then return 'not a source' end
      if mode=='oom' then fixture.fail_next();return string.rep('x',4096) end
      local sent=false
      selected=ok(rotmos.source{read=function()
        if mode=='again' then return nil,rotmos.AGAIN end
        if mode=='empty' then return nil,rotmos.EOF end
        if mode=='read-error' or mode=='read-close-error' then return nil,{status=rotmos.status.INVALID,message='primary read failure'} end
        if sent then return nil,rotmos.EOF end
        sent=true;return pem.certificate
      end,close=function()
        closed=closed+1
        if mode=='close-error' or mode=='read-close-error' then error('close failure') end
      end})
      return selected
    end}}}}}
  local s=ok(rotmos.server(config))
  local value,err=s:validate()
  assert(value==nil and err.status)
  if mode=='read-close-error' or mode=='read-error' or mode=='empty' then assert(err.status==rotmos.status.INVALID) end
  if mode=='again' or mode=='close-error' or mode=='invalid' then assert(err.status==rotmos.status.STATE) end
  if mode=='bound' then assert(err.status==rotmos.status.LIMIT) end
  if mode=='returned-error' then assert(err.status==rotmos.status.CONFLICT and err.message:find('opener returned error',1,true)) end
  if mode=='read-error' or mode=='read-close-error' then assert(err.message:find('primary read failure',1,true)) end
  if mode=='oom' then assert(err.status==rotmos.status.NOMEM) end
  if selected then assert(closed==1);state(selected:read(1)) else assert(closed==0) end
  ok(s:close())
end
-- A failed legacy reset closes the accepted source once without reading it.
do
  local closed=0
  local s=ok(rotmos.server{listeners={{kind='https',identity_index=1}},tls={identities={{domains={'localhost'},
    private_key={bytes=pem.private_key},certificate={open=function() return {
      reset=function() return nil,{status=rotmos.status.CONFLICT,message='legacy reset marker'} end,
      read=function() error('failed reset was read') end,
      close=function() closed=closed+1 end,
    } end}}}}})
  local value,err=s:validate()
  failure(value,err,rotmos.status.CONFLICT);assert(err.message:find('legacy reset marker',1,true))
  assert(closed==1);ok(s:close());assert(closed==1)
end
-- A busy source must stay with its Lua caller when the opener rejects transfer.
local busy,owner,inside
busy=ok(rotmos.source{read=function()
  inside=true;state(owner:validate());return 'x'
end})
owner=ok(rotmos.server{listeners={{kind='https',identity_index=1}},tls={identities={{domains={'localhost'},
  certificate={open=function() return busy end},private_key={bytes=pem.private_key}}}}})
assert(ok(busy:read(1))=='x' and inside);ok(busy:close());ok(owner:close())
local started=0
local close_failure=ok(rotmos.server{workers=1,listeners={{kind='https',identity_index=1}},tls={identities={{domains={'localhost'},
  private_key={bytes=pem.private_key},certificate={open=function()
    local sent=false
    return {read=function() if not sent then sent=true;return pem.certificate end end,close=function() error('late material close') end}
  end}}}},on_worker_event=function() started=started+1 end,on_parent_ready=function() error('failed material became ready') end})
state(close_failure:run());assert(started==0);ok(close_failure:close())
-- ACME callbacks use the actual parent/key-manager boundary. Persistence and
-- Startup does not persist keys; ERROR observers cannot replace the cause.
for _,mode in ipairs{'restore','stop','pending'} do
  local root=fixture.workspace..'/acme-'..mode
  fixture.private_directory(root)
  local reader,writer=fixture.fd_pair()
  local restored,children,observed=0,0,0
  local callback=function(s,event)
    assert(event.runtime_directory==root and type(event.domain)=='string')
    state(s:close());state(s:validate());state(s:run())
    if event.event=='restore' then
      restored=restored+1;assert(event.process.role=='parent' and event.error==nil)
      if mode=='restore' then return nil,{status=rotmos.status.CONFLICT,message='restore marker'} end
      if mode=='stop' then ok(s:stop()) end
      restore_keys(event.runtime_directory)
    elseif event.event=='persist' then
      assert(event.process.role=='key_manager' and event.process.process_id>0 and event.error==nil)
      error('startup must not call PERSIST')
    else
      assert(event.event=='error' and event.error.status~=rotmos.status.OK)
      if mode=='restore' then assert(event.error.status==rotmos.status.CONFLICT)
      else error('unexpected state error') end
      error('ignored ERROR observer failure')
    end
  end
  local config={workers={count=1,death_policy='terminate',shutdown_timeout_ms=2000},
    listeners={{kind='https',identity_index=1,port=0}},tls={identities={{mode='acme',domains={'localhost'}}},
      acme={email='test@example.invalid',directory_url='http://127.0.0.1:9/directory',runtime_directory=root,
        persist_timeout_ms=5000,state=callback}},on_worker_event=function(_,event)
        if event.kind=='started' then children=children+1 end
        if event.kind=='exited' and event.process.role=='key_manager' and event.error.status~=rotmos.status.OK then
          observed=observed+1
        end
      end,
      on_parent_ready=function(s) assert(mode=='pending');ok(s:stop()) end}
  local s=ok(rotmos.server(config))
  assert(restored==0);ok(s:validate());assert(restored==0)
  local effective=ok(s:effective_config());assert(effective.tls.acme.email=='test@example.invalid' and effective.tls.acme.persist_timeout_ms==5000)
  config.tls.acme.state=function() error('mutated state callback') end
  local value,err=s:run()
  if mode=='stop' or mode=='pending' then ok(value,err);assert(not value.has_failure and (mode=='pending' or children==0))
  else assert(value==nil and err.status) end
  assert(restored==1)
  if mode=='restore' then assert(err.status==rotmos.status.CONFLICT and children==0)
  elseif mode=='pending' then assert(children>0 and observed==0) end
  ok(s:close());fixture.fd_close(reader);fixture.fd_close(writer)
end
for n=0,40,4 do
  local success,s=fixture.fail_call(n,function() return rotmos.server{listeners={{kind='https',identity_index=1}},
    tls={identities={{domains={'localhost'},certificate={open=function() return source(pem.certificate,true) end},private_key={bytes=pem.private_key}}}}} end)
  if success and s then ok(s:close()) end
end
collectgarbage('collect')
