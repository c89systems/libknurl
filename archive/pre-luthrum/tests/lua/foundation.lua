local knurl=require('knurl')
assert(host_marker=='preserved')
local function ok(value,err) assert(value,err and (err.message..' '..err.detail));return value end
local invalid,err=knurl.server{port=-1}
assert(invalid==nil and err.status and err.operation and err.source)
assert(knurl.count('18446744073709551615').high==4294967295)
assert(tostring(knurl.count('18446744073709551615'))=='18446744073709551615')
assert(knurl.count('18446744073709551615')==knurl.count(4294967295,4294967295))
assert(knurl.count('18446744073709551614')<knurl.count('18446744073709551615'))
assert(7<knurl.count('18446744073709551615'))
for _,opts in ipairs{{port=0,admission={typo=true}}, {port=0,timeouts={typo=7}}, {port=0,callbacks={on_typo=function() end}}, {port=0,on_parent_ready=false}} do
  local value,why=knurl.server(opts)
  assert(value==nil and why.status==knurl.status.INVALID)
end
assert(select(1,knurl.server{port=0,tls={version='unknown'}})==nil)
-- Startup resource checks share Core diagnostics and receiver lifetime rules.
local probe=ok(knurl.server{port=0,http={access_log_path=fixture.root}})
for name,method in pairs(getmetatable(probe._native).__index) do
  if type(method)=='function' and name:sub(1,2)~='__' then
    assert(type(probe[name])=='function',name..' missing from server receiver')
  end
end
ok(probe:validate())
local passed,cause=probe:preflight()
assert(passed==nil and cause.status==knurl.status.IO and cause.operation=='access_log_open' and cause.detail==fixture.root)
ok(probe:close())
passed,cause=probe:preflight()
assert(passed==nil and cause.status==knurl.status.STATE)
probe=ok(knurl.server{port=0,limits={body_spool_directory=fixture.root..'/missing'}})
ok(probe:post('/spill',function() return 'ok' end,{body={mode='buffered',max_bytes=32,memory_buffer_limit_bytes=16}}))
ok(probe:validate())
passed,cause=probe:preflight()
assert(passed==nil and cause.status==knurl.status.IO and cause.operation=='body_spool')
ok(probe:close())
local source=ok(knurl.source{read=function() return nil,knurl.EOF end})
assert(source:cancelled()==false)
ok(source:close())
local cancelled,closed_error=source:cancelled()
assert(cancelled==nil and closed_error.status==knurl.status.STATE)
probe=ok(knurl.server{port=0,signals={mode='host'},workers={count=1,affinity_disabled=true},http={backend_quiet=true},
  on_parent_ready=function(s) ok(s:notify_signal(15)) end})
assert(#ok(probe:kore_revision())==40)
passed,cause=probe:notify_signal(-1)
assert(passed==nil and cause.status==knurl.status.INVALID)
local signal_result=ok(probe:run())
assert(signal_result.signal_number==15 and signal_result.reason==1 and not signal_result.escalated)
ok(probe:close())
passed,cause=probe:kore_revision()
assert(passed==nil and cause.status==knurl.status.STATE)
passed,cause=probe:notify_signal(15)
assert(passed==nil and cause.status==knurl.status.STATE)
-- Fail allocations throughout C registration; the same owner remains usable.
local native=require('knurl.native')
assert(knurl.version==native.version and type(knurl.version)=='string')
for n=0,18 do
  local owner=ok(native.server{listeners={{port=0}}})
  local spec={path='/allocation',method='GET',handler=function() return 'ok' end}
  fixture.fail_call(n,function() return owner:route(spec) end)
  assert(owner:route_count()<=1)
  ok(owner:close());ok(owner:close())
end
collectgarbage('collect')
local closed,calls,retained,source_closed,skipped=0,0,nil,0,0
local spooled_closed=0
local server=ok(knurl.server{
  port=0,workers={count=1,affinity_disabled=true},http={backend_quiet=true},io={chunk_bytes=7},
  limits={body_spool_directory=fixture.root},
  on_parent_ready=function(s,process)
    assert(process.role=='parent' and process.process_id>0)
    ok(fixture.spawn(ok(s:listener()).port))
  end,
  on_worker_exit=function()
    assert(source_closed==2 and spooled_closed==7 and skipped==0)
  end,
})
assert(calls==0)
ok(server:preflight())
assert(calls==0)
ok(server:get('/text',function(req)
  calls=calls+1;retained=req
  assert(req.method=='GET' and req.path=='/text' and req.ip=='127.0.0.1')
  assert(req.header('absent')==nil and req.header('X-Test')=='value')
  local value,cause=req.query_long('absent')
  assert(value==nil and cause.status==knurl.status.EOF)
  assert(#req.headers>=3 and req.body=='')
  assert(req.body_spooled==false)
  return 'hello'
end))
ok(server:get('/nil',function() end))
ok(server:post('/echo',function(req)
  assert(req.body_size==3)
  if req.header('transfer-encoding') then
    assert(req.trailer('x-trace')=='done' and #req.trailers==1)
    assert(req.header('x-trace')==nil)
  else assert(#req.trailers==0 and req.trailer('x-trace')==nil) end
  return {status=201.0,body=req.body}
end,{body={mode='buffered',max_bytes='16.0'}}))
local group=ok(server:group('/group',{before=function(req) req.group_seen=true end}))
ok(group:get('/value/:id',function(req)
  assert(req.group_seen and req.query('x')=='' and #req.queries==2)
  local value,cause=req.query_boolean('x')
  assert(value==nil and cause.status==knurl.status.INVALID)
  value,cause=req:param_long('id')
  assert(value==42 and cause==nil)
  assert(req.param_u64('id')==42)
  return req.param('id')
end))
local value,why=server:route{path='/literal/:value',path_kind='literal',handler=function() end}
assert(value==nil and why.status==knurl.status.INVALID)
ok(server:get('/before',function() skipped=skipped+1 end,{before=function() return {status=403,body='blocked'} end,after=function() error('after short circuit') end}))
ok(server:get('/after',function() return {body='original',headers={['X-First']='one'}} end,{after=function(_,reply) reply.status_code=0.0;reply.status='202';reply.body='changed';reply.headers['X-After']='yes' end}))
ok(server:get('/throw',function() error('expected protected handler error') end))
ok(server:get('/stream',function()
  local state={n=0}
  return {stream_source={
    reset=function() assert(state.n==0);state.reset=true end,
    read=function(capacity)
      assert(capacity<=7 and state.reset);state.n=state.n+1
      if state.n==1 then return string.rep('s',1000) end
      if state.n==2 then return 123 end
      if state.n==3 then return '' end
      error('source read past EOF')
    end,
    close=function() source_closed=source_closed+1;assert(state.n==3) end,
  }}
end))
ok(server:get('/source-error',function() return {stream_source={read=function() error('expected source failure') end,close=function() source_closed=source_closed+1 end}} end))
ok(server:route{path='/spooled',methods={'GET','HEAD'},handler=function()
  local state={n=0,resets=0}
  return {spooled_source={
    reset=function() state.resets=state.resets+1 end,
    read=function(capacity)
      assert(capacity==8192 and state.resets==1);state.n=state.n+1
      if state.n==1 then return string.rep('p',20000)..'\0' end
      if state.n==2 then return 123 end
      return nil
    end,
    close=function()
      spooled_closed=spooled_closed+1;assert(state.n==3 and state.resets==1)
      local value,why=server:close();assert(value==nil and why.status==knurl.status.STATE)
    end,
  }}
end})
ok(server:get('/spooled-error',function() return {spooled_source={read=function() error('expected spool read failure') end,close=function() spooled_closed=spooled_closed+1 end}} end))
ok(server:get('/spooled-oom',function() return {spooled_source={read=function() fixture.fail_next();return string.rep('x',30000) end,close=function() spooled_closed=spooled_closed+1 end}} end))
ok(server:get('/spooled-close-error',function() return {spooled_source={read=function() return false end,close=function() spooled_closed=spooled_closed+1;error('expected spool close failure') end}} end))
ok(server:get('/spooled-strict',function()
  local n=0
  return {spooled_source=ok(knurl.source{
    read=function() n=n+1;if n==1 then return 'strict' end;return nil,knurl.EOF end,
    close=function(cause) assert(cause==knurl.status.OK and n==2);spooled_closed=spooled_closed+1 end,
  })}
end))
ok(server:get('/spooled-paused',function() return {spooled_source=ok(knurl.source{
  read=function() return nil,knurl.AGAIN end,
  close=function(cause) assert(cause==knurl.status.CANCELLED);spooled_closed=spooled_closed+1 end,
})} end))
ok(server:get('/spooled-discard',function() return {spooled_source=setmetatable({},{__index=function() error('unselected spool getter') end})} end,
  {after=function() return {body='discarded'} end}))
for _,phase in ipairs{'before','after'} do
  for _,value in ipairs{false,'invalid'} do
    local hooks={[phase]=function() return value end}
    ok(server:get('/hook-'..phase..'-'..tostring(value),function()
      if phase=='before' then error('before must short circuit') end
      return 'discarded'
    end,hooks))
  end
end
ok(server:get('/continue',function() return {status_code=100,body='suppressed'} end))
ok(server:get('/hints',function() return {status_code=103,body='suppressed'} end))
ok(server:get('/getter-error',function() return setmetatable({},{__index=function() error('expected getter failure') end}) end))
local reply={body='cannot allocate this reply'}
ok(server:get('/oom-reply',function() fixture.fail_next();return reply end))
ok(server:get('/oom-source',function()
  return {stream_source=setmetatable({},{__index=function(_,key)
    if key=='read' then fixture.fail_next();return function() return nil end end
  end})}
end))
local untouched={stream_source=setmetatable({},{__index=function() error('unselected source getter') end})}
ok(server:get('/discard',function() return untouched end,{after=function(_,value)
  assert(value==untouched)
  return {status_code=203,status=599,body='replacement',headers={['X-Number']=17}}
end}))
assert(server:group{
  path_prefix='/table-group',method='GET',body=false,
  before=function(req) req.default_seen=true end,
  routes={
    {path='entry',handler=function(req) assert(req.default_seen);return 'default' end},
    {path='other',methods={'POST','PUT'},before=function(req) assert(not req.default_seen) end,
     handler=function() return 'override' end},
  },
}==true)
ok(server:get('/verify',function()
  local value,expired=retained.header('x')
  assert(value==nil and expired.kind=='invalid state')
  assert(calls==1 and source_closed==2 and spooled_closed==7 and skipped==0)
  return 'verified'
end))
ok(server:get('/stop',function() ok(server:stop());return 'stopped' end))
assert(server:route_count()==30)
local value,why=server:route{path='/bad-method',method='BOGUS',handler=function() end}
assert(value==nil and why.status==knurl.status.INVALID and server:route_count()==30)
value,why=server:route{path='/auth',auth={},handler=function() end}
assert(value==nil and why.status==knurl.status.NOT_SUPPORTED)
ok(server:validate())
ok(server:run())
ok(fixture.join())
ok(server:close());ok(server:close())
local value,expired=server:get('/closed',function() end)
assert(value==nil and expired.status)
-- GC closes an untransferred provider once, without invoking read.
do
  local src=ok(knurl.source{read=function() error('read during GC') end,close=function() closed=closed+1 end})
  src=nil
end
collectgarbage('collect');collectgarbage('collect');assert(closed==1)
local broken=ok(knurl.source{read=function() return nil,knurl.EOF end,close=function() error('expected close failure') end})
local value,why=broken:close()
assert(value==nil and why.message:find('expected close failure',1,true))
assert(host_marker=='preserved')
