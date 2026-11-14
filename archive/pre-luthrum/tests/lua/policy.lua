local knurl=require('knurl')
local function ok(value,err) if not value then error(err and err.message or 'operation failed') end;return value end
local function state(value,err) assert(value==nil and err.status==knurl.status.STATE) end
local pem=fixture.tls_setup(fixture.root..'/policy-key.pem')
local starts,closes,completed,handled,connections,events=0,0,0,0,0,0
local worker_started,worker_exited,tls_usable=0,0,false
local retained,saved_observation,saved_worker
local opts={port=0,workers=1,listeners={{port=0},{kind='https',port=0,identity_index=1}},
  tls={identities={{domains={'localhost'},certificate={bytes=pem.certificate},private_key={bytes=pem.private_key}}}},
  client_ip={trusted_proxies={'127.0.0.1'},resolve=function(req)
    assert(req.peer.address=='127.0.0.1')
    req.flow={resolved=true}
    if req.path=='/bad-ip' then return 'not an address' end
    if req.path=='/ip-nul' then return '203.0.113.1\0hidden' end
    if req.path=='/throw-resolver' then error('resolver failure') end
    if req.path=='/fallback' then return nil end
    return '203.0.113.42'
  end},
  correlation_id={resolve=function(req)
    assert(req.flow.resolved)
    if req.path=='/fallback' then assert(req.ip=='198.51.100.7');return nil end
    assert(req.ip=='203.0.113.42')
    if req.path=='/bad-correlation' then return 'invalid\nidentifier' end
    if req.path=='/long-id' then return string.rep('x',129) end
    return req.path
  end},
  admission={on_connection=function(info)
    connections=connections+1
    assert(info.peer.address=='127.0.0.1' and info.local_endpoint.port>0 and info.process.role=='http')
    assert(info.listener_index==1 or (info.listener_index==2 and info.tls))
    if connections==1 then return 'reject' end
    return true
  end,on_request=function(req)
    assert(req.flow.started)
    req.flow.admitted=true
    if req.path=='/close' then return 'close' end
    if req.path=='/reject' then return {action='reject',status=429,headers={['X-Admission']='copied'},body='R\0J'} end
    if req.path=='/reject-default' then return false end
    if req.path=='/admit-bad' then return 'unknown' end
    if req.path=='/admit-source' then return {action='reject',stream_source={read=function() error('unselected source read') end,
      close=function() error('unselected source closed') end}} end
    if req.path=='/admit-getter' then return setmetatable({},{__index=function() error('decision getter') end}) end
    if req.path=='/admit-oom' then fixture.fail_next();return {action='reject',body=string.rep('x',4096)} end
    return {action='allow'}
  end},callbacks={
    on_request_start=function(_,req)
      starts=starts+1;assert(req.flow.resolved)
      req.flow.started=true
      if req.path=='/start-fails' then return nil,{status=knurl.status.STATE,message='start failed'} end
      assert(req.correlation_id==(req.path=='/fallback' and '' or req.path))
    end,
    on_request_close=function(_,req,outcome)
      closes=closes+1
      assert(req.flow.resolved and req.observed_complete and req.header('x-test')~='unavailable')
      assert(type(outcome.response.wire_bytes)=='number' and type(outcome.response.duration_ms)=='number')
      if req.path=='/echo' then
        assert(req==retained and req.body=='A\0B' and outcome.body_complete)
        assert(outcome.received_body_bytes==3 and outcome.delivered_body_bytes==3)
        assert(outcome.response.status==200 and outcome.response.body_bytes==3 and outcome.response.committed)
        assert(outcome.response.wire_bytes>3)
      end
      req.closed=true
    end,
    on_observe=function(_,event)
      assert(event.connection.peer.address=='127.0.0.1')
      if not event.request then
        assert(event.effective_address==nil and not event.has_outcome)
        return
      end
      assert(event.request.flow.resolved and event.effective_address==event.request.ip)
      if event.kind=='application' then
        events=events+1;assert(event.name=='named.event')
        state(event.request.event('recursive'))
      elseif event.kind=='request_complete' then
        completed=completed+1
        assert(event.has_outcome and event.outcome.response.status==event.status)
        event.request.observed_complete=true
        if event.request.path=='/echo' then assert(event.input_counts_known);saved_observation=event end
        if event.request.path=='/reject' then assert(not event.input_counts_known) end
      else assert(not event.has_outcome and event.outcome==nil) end
    end,
    on_worker_event=function(_,event)
      assert(event.process.role=='http' and event.process.process_id>0)
      if event.kind=='started' then worker_started=worker_started+1;saved_worker=event end
      if event.kind=='exited' then worker_exited=worker_exited+1;assert(event.expected and event.exit_status==0) end
    end,
    on_tls_identity=function(s,event)
      assert(event.identity_index==1 and event.error.status==knurl.status.OK)
      if event.state=='usable' then tls_usable=true;assert(ok(s:tls_identity_state())=='usable') end
    end,
  }}
opts.on_parent_ready=function(s)
  assert(ok(s:tls_identity_state())=='usable')
  ok(fixture.spawn_observations(ok(s:listener(1)).port,ok(s:listener(2)).port))
end
opts.on_worker_exit=function()
  assert(starts==14 and closes==19 and completed==19 and handled==6 and events==1 and connections==20)
  assert(saved_observation.request.closed and saved_observation.outcome.response.body_bytes==3)
end
local server=ok(knurl.server(opts))
state(server:tls_identity_state())
ok(server:validate());assert(starts==0 and connections==0 and worker_started==0 and not tls_usable)
opts.client_ip.resolve=function() error('mutated resolver') end
opts.correlation_id.resolve=function() error('mutated correlation') end
opts.admission.on_request=function() error('mutated admission') end
opts.callbacks.on_request_start=function() error('mutated lifecycle') end
for _,path in ipairs{'/echo','/fallback','/before','/event','/tls','/stop'} do
  ok(server:route{path=path,methods={'GET','POST'},body={mode='buffered',max_bytes=4096,memory_buffer_limit_bytes=4096},before=function(req)
    assert(req.flow.admitted)
    if path=='/before' then assert(req.body=='original');req.body='changed' end
  end,handler=function(req)
    handled=handled+1
    if path=='/echo' then retained=req;assert(req.body=='A\0B');return req.body end
    if path=='/before' then assert(req.body=='changed');return req.body end
    if path=='/event' then ok(req.event('named.event'));return 'event' end
    if path=='/tls' then assert(req.tls);return 'tls' end
    if path=='/stop' then
      assert(retained.closed);state(retained.header('x-test'))
      ok(server:stop());return 'ok'
    end
    return 'fallback'
  end})
end
local result=ok(server:run());ok(fixture.join())
assert(not result.has_failure and result.recovered_failures==0)
assert(worker_started==1 and worker_exited==1 and tls_usable and saved_worker.kind=='started')
state(server:tls_identity_state());ok(server:close())
-- The descriptor belongs to the server only after a successful operation.
for _,mode in ipairs{'declaration','take','eof','returned-error','throw'} do
  local reader,writer=fixture.fd_pair()
  local called,eof_events=0,0
  local config={enabled=true,control_fd=reader,on_event=function(s,flags)
    called=called+1;assert(flags & knurl.control.READ~=0 or flags & knurl.control.EOF~=0)
    if flags & knurl.control.EOF~=0 then eof_events=eof_events+1 end
    local bytes=fixture.fd_read(reader)
    if mode=='returned-error' then return nil,{status=knurl.status.INVALID,message='control returned error'} end
    if mode=='throw' then error('control callback failed') end
    if mode=='eof' then assert(bytes=='');return end
    assert(bytes=='wake');ok(s:control_interest(0));ok(s:stop())
  end}
  local s=ok(knurl.server{port=0,workers=1,supervision=mode~='take' and config or nil,
    on_process_start=function(_,process) if process.role~='parent' then fixture.fd_close(writer) end end,
    on_parent_ready=function()
      if mode=='eof' then fixture.fd_close(writer);writer=nil else fixture.fd_write(writer,'wake') end
    end})
  if mode=='take' then ok(s:supervision_take(config)) end
  if mode=='declaration' then
    state(s:supervision_take{enabled=true,control_fd=reader,on_event=function() error('failed take replaced callback') end})
  end
  config.on_event=function() error('mutated supervision') end
  local snapshot=ok(s:effective_config());assert(snapshot.supervision.enabled and snapshot.supervision.control_fd==reader)
  local value,err=s:run()
  if mode=='returned-error' or mode=='throw' then assert(value==nil and err.status and called==1)
  else
    ok(value,err)
    -- kqueue may report READ before EOF even when the pipe is empty.
    if mode=='eof' then assert(eof_events==1 and called>=1 and called<=2)
    else assert(called==1 and eof_events==0) end
  end
  assert(fixture.fd_closed(reader));ok(s:close())
  if writer then fixture.fd_close(writer) end
end
for n=0,24,4 do
  local reader,writer=fixture.fd_pair()
  local s=ok(knurl.server{port=0})
  local success,taken=fixture.fail_call(n,function() return s:supervision_take{enabled=true,control_fd=reader,on_event=function() end} end)
  ok(s:close())
  if success and taken then assert(fixture.fd_closed(reader))
  else assert(not fixture.fd_closed(reader));fixture.fd_close(reader) end
  fixture.fd_close(writer)
end
local reader,writer=fixture.fd_pair()
local invalid={port=0,supervision={enabled=true,control_fd=reader,interest=128,on_event=function() end}}
local value,err=knurl.server(invalid);assert(value==nil and err.status);assert(not fixture.fd_closed(reader))
fixture.fd_close(reader);fixture.fd_close(writer)
-- Void callback errors cannot skip request retirement or escape into the core.
for _,phase in ipairs{'on_observe','on_request_close','on_worker_event'} do
  local saved,body_reader
  local callbacks={}
  callbacks[phase]=function(_,value)
    if phase=='on_worker_event' then error('worker event callback') end
    if phase=='on_observe' and value.kind~='request_complete' then return end
    saved=phase=='on_observe' and value.request or value
    if phase=='on_request_close' then fixture.fail_next();return string.rep('x',4096) end
    error('observation callback')
  end
  local s=ok(knurl.server{port=0,workers=1,callbacks=callbacks,
    on_parent_ready=phase~='on_worker_event' and function(owner) ok(fixture.spawn_single(ok(owner:listener(1)).port)) end or nil,
    on_worker_exit=function()
      if saved then state(saved.header('x-test')) end
      if body_reader then state(body_reader:read(1));ok(body_reader:close()) end
    end})
  ok(s:get('/',function(req) body_reader=ok(req.body_reader());return 'ok' end,
    {body={mode='buffered',max_bytes=16,memory_buffer_limit_bytes=16}}))
  value,err=s:run();assert(value==nil and err.status)
  if phase~='on_worker_event' then ok(fixture.join()) end
  ok(s:close())
end
for _,invalid in ipairs{
  {port=0,client_ip={resolve=true}}, {port=0,correlation_id={resolve=1}},
  {port=0,admission={on_request='bad'}}, {port=0,supervision={enabled=1}},
  {port=0,callbacks={on_observe=1}}, {port=0,admission={unknown=true}},
} do value,err=knurl.server(invalid);assert(value==nil and err.status==knurl.status.INVALID) end
collectgarbage('collect')
