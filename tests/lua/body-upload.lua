local knurl=require('knurl')
local native=require('knurl.native')
fixture.prepare_body()
local function ok(value,err) assert(value,err and (err.message..' '..err.detail));return value end
local function state(value,err) assert(value==nil and err.status==knurl.status.STATE) end
local function bytes(reader)
  local chunks={}
  while true do
    local chunk,err=reader:read(2)
    if not chunk then assert(err==knurl.EOF);break end
    chunks[#chunks+1]=chunk
  end
  return table.concat(chunks)
end
for n=0,22 do
  local owner=ok(native.server{listeners={{port=0}}})
  fixture.fail_call(n,function() return owner:upload{path='/allocation',on_chunk=function() end} end)
  assert(owner:route_count()<=1)
  ok(owner:close())
end
collectgarbage('collect')
local saved,artifact,borrowed,transport_path,result_reader
local closes,opens,completes,source_closes=0,0,0,0
local server=ok(knurl.server{
  port=0,workers={count=1,affinity_disabled=true},http={backend_quiet=true},io={chunk_bytes=2},
  limits={body_spool_directory=fixture.root},
  on_parent_ready=function(s) ok(fixture.spawn_body(ok(s:listener()).port)) end,
  on_worker_exit=function() assert(closes==9 and opens==9 and completes==1 and source_closes==1) end,
})
ok(server:post('/materialized',function(req)
  assert(req.body==nil and req.body_spooled and req.body_size==6 and not req.body_streaming_upload)
  transport_path=req.body_path;assert(type(transport_path)=='string')
  state(req:body_bytes())
  borrowed=ok(req:body_reader())
  assert(bytes(borrowed)=='A\0BCDE');ok(borrowed:reset())
  saved=ok(req.body_copy(8))
  local info=ok(saved:snapshot());assert(info.kind=='memory' and info.length==6 and info.memory=='A\0BCDE')
  artifact=ok(req:body_spill{directory=fixture.root,prefix='lua-copy',memory_limit_bytes=2})
  assert(ok(artifact:snapshot()).kind=='file')
  local reader=ok(artifact:reader())
  state(artifact:close());assert(bytes(reader)=='A\0BCDE')
  ok(reader:reset());assert(bytes(reader)=='A\0BCDE');ok(reader:close())
  local copy=ok(req:body_materialize{capacity=7,memory_limit_bytes=7})
  assert(ok(copy:snapshot()).memory=='A\0BCDE');ok(copy:close())
  copy=ok(req:body_materialize());ok(copy:close())
  local missing,why=req:body_spill{directory=fixture.root..'/absent',memory_limit_bytes=1}
  assert(missing==nil and why.status==knurl.status.IO)
  local owned=ok(req:body_copy())
  local pinned=ok(owned:reader());owned=nil;collectgarbage('collect')
  assert(bytes(pinned)=='A\0BCDE');ok(pinned:close())
  return {stream_source=ok(req:body_reader())}
end,{body={mode='materialized',max_bytes=100,memory_buffer_limit_bytes=2}}))
ok(server:get('/saved',function()
  state(borrowed:read());state(borrowed:reset());ok(borrowed:close())
  assert(io.open(transport_path,'rb')==nil)
  result_reader=ok(saved:reader());state(saved:close())
  return {stream_source=result_reader}
end))
ok(server:get('/check-saved',function()
  state(result_reader:read());ok(saved:close());state(saved:snapshot())
  local path=ok(artifact:snapshot()).path
  ok(artifact:close());local file=ok(io.open(path,'rb'));assert(file:read('a')=='A\0BCDE');file:close()
  ok(os.remove(path));return 'ok'
end))
local group=ok(server:group('/upload',{after=function(req,response)
  assert(req.order=='boca');req.order=req.order..'g';return response
end}))
ok(group:upload{
  path='/live',methods={'POST','PUT'},buffer_bytes=1,
  before=function(req)
    assert(req.body==nil and req.body_streaming_upload);state(req.body_reader())
    req.order='b'
  end,
  on_open=function(req) opens=opens+1;req.order=req.order..'o';return {n=0} end,
  write=function(req,chunk,s)
    assert(#chunk<=1 and req.order=='bo')
    if s.n==0 then fixture.upload_progress() end
    s.n=s.n+#chunk
  end,
  finish=function(req,s)
    completes=completes+1;assert(s.n==6);req.order=req.order..'c'
    local sent=false
    return {stream_source={
      reset=function() assert(s.closed and req.order=='bocag') end,
      read=function() if not sent then sent=true;return s.n end end,
      close=function() source_closes=source_closes+1;assert(s.closed and sent) end,
    }}
  end,
  after=function(req,response) assert(req.order=='boc');req.order=req.order..'a';return response end,
  on_close=function(req,s) assert(req.order=='bocag');s.closed=true;closes=closes+1 end,
})
ok(server:upload{path='/empty',on_chunk=function() error('empty upload delivered data') end,
  close=function(req,s) assert(type(s)=='table' and req.body_streaming_upload);closes=closes+1 end})
ok(server:upload{path='/paused',buffer_bytes=3,body={max_bytes=100},
  open=function() opens=opens+1;return {n=0} end,
  on_chunk=function(req,chunk,s)
    if not s.paused then s.paused=true;ok(req.body_resume());return nil,knurl.AGAIN end
    local n=math.min(1,#chunk);s.n=s.n+n;return n
  end,
  on_complete=function(_,s) assert(s.n==6);return tostring(s.n) end,
  close=function() closes=closes+1 end,
})
for _,phase in ipairs{'open','chunk','oom','finish','getter','close','invalid'} do
  ok(server:upload{path='/failure/'..phase,
    open=function()
      opens=opens+1
      if phase=='open' then error('expected open failure') end
      return {}
    end,
    chunk=function()
      if phase=='chunk' then error('expected chunk failure') end
      if phase=='oom' then fixture.fail_next();return string.rep('x',30000) end
      if phase=='invalid' then return false end
    end,
    complete=function()
      if phase=='finish' then error('expected completion failure') end
      if phase=='getter' then return setmetatable({},{__index=function() error('expected response getter failure') end}) end
      return 'success'
    end,
    close=function() closes=closes+1;if phase=='close' then error('expected close failure') end end,
  })
end
ok(server:upload{path='/limit',max_body_bytes=2,on_chunk=function() error('over-limit upload opened') end,
  open=function() error('over-limit upload opened') end})
ok(server:upload{path='/denied',auth={kind='callback',callback=function() return {decision='deny',body=''} end},
  on_chunk=function() error('denied upload opened') end,open=function() error('denied upload opened') end})
local invalid,why=server:upload{path='/invalid',chunk=false}
assert(invalid==nil and why.status==knurl.status.INVALID)
invalid,why=server:upload{path='/invalid',open=false,on_open=function() end,chunk=function() end}
assert(invalid==nil and why.status==knurl.status.INVALID)
ok(server:get('/stop',function() assert(closes==9 and opens==9);ok(server:stop());return 'ok' end))
ok(server:validate());ok(server:run());ok(fixture.join());ok(server:close())
