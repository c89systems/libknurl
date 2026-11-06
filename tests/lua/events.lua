local rotmos=require('rotmos')
local native=require('rotmos.native')
local function ok(value,err) assert(value,err and (err.message..' '..err.detail));return value end
local function state(value,err) assert(value==nil and err.status==rotmos.status.STATE) end
assert(ok(rotmos.sse_encode{data='A\0B\nC'})=='data: A\0B\ndata: C\n\n')
assert(ok(rotmos.sse_encode{retry=rotmos.count('18446744073709551615')})=='retry: 18446744073709551615\n\n')
assert(ok(rotmos.sse_encode(setmetatable({frame='raw'},{__index=function() error('unselected event getter') end}),3))=='raw')
for _,event in ipairs{{retry=-1},{retry=0.5},{retry=1.0},{retry='1'},{data={}},{frame={}}} do
  local value,err=rotmos.sse_encode(event);assert(value==nil and err.status==rotmos.status.INVALID)
end
local value,err=rotmos.sse_encode('too big',2);assert(value==nil and err.status==rotmos.status.LIMIT)
for n=0,24,4 do
  local owner=ok(native.server{listeners={{port=0}}})
  fixture.fail_call(n,function() return owner:websocket{path='/allocation',message=function() end,headers={['X-Test']='value'}} end)
  assert(owner:route_count()<=1);ok(owner:close())
end
collectgarbage('collect')
local readfd=fixture.events_pipe()
local material=fixture.tls_setup(fixture.workspace..'/events.key')
local retired,watched,woken,repeats,callback_failures,sse_closed,opens,closes,writes,pauses,group_after=0,0,0,0,0,0,0,0,0,0,0
local retained,retained_request,error_connections,per_ws=nil,nil,{},{}
local parent_token
local server=ok(rotmos.server{
  listeners={{port=0},{kind='https',port=0,identity_index=1}},
  tls={identities={{domains={'localhost'},certificate={kind='memory',bytes=material.certificate},private_key={kind='memory',bytes=material.private_key},client_ca={kind='memory',bytes=material.ca},require_client_certificate=true}}},
  workers={count=1,affinity_disabled=true},http={backend_quiet=true},
  io={output_queue_bytes=127,chunk_bytes=64},
  on_parent_prepare=function(s) parent_token=ok(s:waker(function() error('parent waker delivered') end)) end,
  on_worker_start=function(s)
    state(parent_token:signal());ok(parent_token:close())
    assert(type(ok(s:now()))=='number')
    for _,kind in ipairs{'defer','timer','watch','waker'} do
      for n=0,20,4 do
        local declaration={callback=function() end}
        if kind=='watch' then declaration.fd=readfd;declaration.interest=0 end
        local success,token=fixture.fail_call(n,function() return s[kind](s,declaration) end)
        if success and token then ok(token:close()) end
      end
    end
    local cancelled=ok(s:defer{callback=function() error('cancelled defer delivered') end,retired=function(token,e) assert(e==nil);ok(token:close());retired=retired+1 end})
    ok(cancelled:close());ok(cancelled:close())
    ok(s:defer(function() retired=retired+1 end))
    ok(s:watch{fd=readfd,interest=rotmos.loop.READABLE,callback=function(token,flags)
      assert(flags%2==1);fixture.events_read();watched=watched+1;ok(token:update(0));ok(token:close())
    end,retired=function(token,e) assert(e==nil);state(token:update(1));retired=retired+1 end})
    local wake=ok(s:waker{callback=function(token) woken=woken+1;ok(token:close()) end,
      retired=function(token,e) assert(e==nil);state(token:signal());retired=retired+1 end})
    ok(wake:signal());ok(wake:signal())
    ok(s:timer{repeat_ms=1,callback=function(token) repeats=repeats+1;if repeats==2 then ok(token:close()) end end,
      retired=function(_,e) assert(e==nil);retired=retired+1 end})
    for _,phase in ipairs{'error','oom'} do
      ok(s:timer{callback=function()
        if phase=='error' then error('expected loop callback failure') end
        fixture.fail_next();return string.rep('x',30000)
      end,retired=function(token,e)
        callback_failures=callback_failures+1
        assert(e and e.status==(phase=='error' and rotmos.status.STATE or rotmos.status.NOMEM))
        local v,why=token:close();assert(v==nil and why.status==e.status)
      end})
    end
  end,
  on_parent_ready=function(s) ok(fixture.spawn_events(ok(s:listener(1)).port,ok(s:listener(2)).port)) end,
  on_worker_exit=function() assert(opens==6 and closes==6 and pauses==2 and writes==1 and sse_closed==2) end,
})
state(server:now());state(server:defer(function() end))
ok(server:sse{path='/sse',status_code='0.0',status=200.0,headers={{name='X-SSE',value='copied'}},
  open=function(req) retained_request=req;return {n=0,request=req} end,
  read=function(s,capacity)
    assert(capacity>0)
    s.n=s.n+1
    if s.n==1 then ok(server:defer(function() ok(s.request:producer_resume()) end));return nil,rotmos.AGAIN end
    if s.n==2 then return 'A\0B\nC' end
    if s.n==3 then return {comment='c',id='1',event='ready',retry=0,data='done'} end
    return false
  end,close=function(s) assert(s.n==4);sse_closed=sse_closed+1 end,
})
local group=ok(server:group('/group',{after=function(_,response) group_after=group_after+1;assert(response.content_type=='text/event-stream; charset=utf-8') end}))
ok(group:sse{path='/sse',open=function() return {} end,read=function(s) if not s.sent then s.sent=true;return 'g' end end,
  close=function(s) assert(s.sent);sse_closed=sse_closed+1 end})
ok(server:sse{path='/sse-denied',auth={kind='callback',callback=function() return {action='deny',body='no'} end},
  open=function() error('denied SSE opened') end,read=function() error('denied SSE read') end})
local spec={path='/ws',protocol='test',headers={['X-Handshake']='copied'},max_frame_bytes=256,
  connect=function(ws)
    opens=opens+1;assert(ws:state()=='open');per_ws[ws]={};retained=ws
    ok(ws:send_text('ready'));ok(ws:send(0));ok(ws:send_text('\xc0\xaf'));ok(ws:send_binary(string.rep('w',512)))
    ok(ws:close());assert(ws:state()=='open');ok(ws:send_text('after-close'))
  end,
  handler=function(ws,opcode,payload)
    local s=per_ws[ws];assert(s and ws==retained)
    if payload=='pause' and not s.paused then
      s.paused=true;pauses=pauses+1;ok(server:defer(function() ok(ws:resume()) end));return nil,rotmos.AGAIN
    end
    ok(ws:send(opcode,payload))
  end,
  disconnect=function(ws,out) assert(out.opened and out.clean and out.peer_code==1000);closes=closes+1;per_ws[ws]=nil end,
}
ok(server:websocket(spec));spec.headers['X-Handshake']='mutated';spec.protocol='mutated'
ok(server:websocket{path='/pressure',max_frame_bytes=256,output_queue_bytes=127,
  connect=function(ws)
    opens=opens+1;ok(ws:send_binary(string.rep('x',100)))
    local v,why=ws:send_binary(string.rep('x',100));assert(v==nil and why==rotmos.AGAIN and ok(ws:queued_bytes())==102)
  end,
  message=function() end,writable=function(ws) writes=writes+1;ok(ws:send_binary(string.rep('x',100))) end,
  disconnect=function(_,out) assert(out.clean);closes=closes+1 end,
})
for _,phase in ipairs{'error','oom','close-error'} do
  ok(server:websocket{path='/ws-'..phase,
    connect=function(ws) opens=opens+1;error_connections[#error_connections+1]=ws end,
    message=function(ws,opcode,payload)
      if phase=='error' then error('expected frame failure') end
      if phase=='oom' then fixture.fail_next();return string.rep('x',30000) end
      ok(ws:send(opcode,payload))
    end,
    disconnect=function(_,out)
      closes=closes+1
      if phase=='close-error' then error('expected disconnect failure') end
      assert(out.error and out.error.status==(phase=='error' and rotmos.status.STATE or rotmos.status.NOMEM))
    end,
  })
end
ok(server:get('/stop',function()
  assert(opens==6 and closes==6 and pauses==2 and writes==1 and sse_closed==2 and group_after==1)
  assert(watched==1 and woken==1 and repeats==2 and retired==5 and callback_failures==2)
  assert(retained:state()=='closed');state(retained:send_text('late'));ok(retained:close())
  for _,ws in ipairs(error_connections) do
    assert(ws:state()=='closed');state(ws:send_text('late'))
    local v,why=ws:close();assert(v==nil and (why.status==rotmos.status.STATE or why.status==rotmos.status.NOMEM))
  end
  state(retained_request:producer_resume());ok(server:stop());return 'ok'
end))
ok(server:validate());ok(server:run());ok(fixture.join());ok(server:close());fixture.events_cleanup();ok(os.remove(fixture.workspace..'/events.key'))
