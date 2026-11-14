local knurl=require('knurl')
local curl=require('async.curl')
local sync=require('curl')
local adapter=require('knurl_async_fixture')
local json=require('lonejson').new()
local function ok(value,err) assert(value,err and (type(err)=='table' and err.message or tostring(err)));return value end
assert(type(curl.version())=='string' and curl.core==require('async.curl.core'))
assert(sync.core==require('curl.core') and sync.core~=curl.core)
local upstream=fixture.curl_setup()
for _,options in ipairs{
  {url=upstream,typo=true}, {url='a\0b'}, {url=upstream,timeout_ms=-1},
  {url=upstream,headers={['Bad\0name']='value'}},
  {url=upstream,headers={name='bad\r\nInjected: true'}},
  {url=upstream,body='body',multipart={field='value'}},
  {url=upstream,read=true}, {url=upstream,smtp={mail_from='bad\r\n',rcpt={'a@b'}}},
  {url=upstream,multipart={['bad\0name']='value'}},
} do assert(not pcall(sync.perform,options)) end
assert(not pcall(curl.perform,{url=upstream}))
assert(not pcall(adapter.run,0))
assert(not pcall(knurl.async_configure,{threads=9}))
assert(not pcall(knurl.async_configure,{max_tasks=257}))
assert(not pcall(knurl.async_configure,{typo=1}))
ok(knurl.async_configure{threads=2,max_tasks=16})
collectgarbage('collect')
local schema=json.schema('response',{json.field('message',json.string{required=true})})
local cancelled,continued,slow,batch_closed,released=0,0,0,0,false
local chunks,stream_finished=0,false
local pair_started,pair_finished,pair_release=0,0,false
local server=ok(knurl.server{port=0,workers={count=1,affinity_disabled=true},http={backend_quiet=true},
  on_parent_ready=function(s) ok(fixture.spawn_curl(ok(s:listener()).port)) end,
  on_request_close=function(_,req)
    if req.path=='/cancel' then cancelled=cancelled+1;fixture.curl_cancelled() end
    if req.path=='/batch' then batch_closed=batch_closed+1;fixture.curl_cancelled() end
  end})
ok(server:get('/slow',function()
  local r=curl.perform{url=upstream..'/held'}
  assert(r.ok and r.status==200 and r.body=='held');slow=slow+1
  return r.body
end))
ok(server:get('/fast',function() assert(slow==0);return 'fast' end))
ok(server:get('/pair-origin',function()
  pair_started=pair_started+1;fixture.curl_pair_started()
  while not pair_release do ok(knurl.sleep(1)) end
  return 'paired'
end))
ok(server:get('/pair',function()
  local url='http://127.0.0.1:'..ok(server:listener()).port..'/pair-origin'
  local r=curl.perform{url=url}
  assert(r.ok and r.body=='paired');pair_finished=pair_finished+1;return 'ok'
end))
ok(server:get('/pair-release',function()
  assert(pair_started==2 and pair_finished==0);pair_release=true;return 'ok'
end))
ok(server:get('/stream',function()
  local r=curl.perform{url=upstream..'/stream',write=function(bytes)
    chunks=chunks+1
    assert(bytes==(chunks==1 and 'one' or 'two'))
    if chunks==1 then fixture.curl_streamed() end
    return true
  end}
  assert(r.ok and r.streamed and r.body=='' and chunks==2)
  stream_finished=true;return 'ok'
end))
ok(server:get('/stream-progress',function() assert(chunks==1 and not stream_finished);return 'ok' end))
ok(server:get('/adapter',function()
  local status,calls,why=adapter.run(0)
  assert(status==knurl.status.OK and calls==1,why)
  status,calls,why=adapter.run(1)
  assert(status==knurl.status.IO and calls==0 and why:find('native-error-marker',1,true))
  status,calls,why=adapter.run(2)
  assert(status==knurl.status.TIMEOUT and calls==0,why)
  status,calls,why=adapter.run(3)
  assert(status==knurl.status.IO and calls==1 and why:find('owner-callback-marker',1,true))
  status,calls,why=adapter.run(4)
  assert(status==knurl.status.CANCELLED and calls==1,why)
  assert(not pcall(knurl.async_configure,{threads=1}))
  return 'ok'
end))
ok(server:get('/files',function()
  local input,output=fixture.root..'/curl-upload',fixture.root..'/curl-download'
  local file=assert(io.open(input,'wb'));assert(file:write('onetwo'));assert(file:close())
  local r=curl.perform{url=upstream..'/file-upload',method='POST',upload_path=input}
  assert(r.ok and r.body=='read',r.error)
  r=curl.perform{url=upstream..'/json',download_path=output}
  assert(r.ok and r.streamed and r.body=='',r.error)
  file=assert(io.open(output,'rb'));assert(file:read('a')=='{"message":"hello"}');assert(file:close())
  assert(not curl.perform{url=upstream,upload_path=fixture.root}.ok)
  assert(not curl.perform{url=upstream,download_path=fixture.root}.ok)
  assert(os.remove(input));assert(os.remove(output));return 'ok'
end))
ok(server:get('/json',function()
  local r=curl.json{url=upstream..'/json'}
  assert(r.ok and r.json.message=='hello');return 'ok'
end))
ok(server:get('/json-map',function()
  local r=curl.stream_json{url=upstream..'/json',response={schema=schema}}
  assert(r.ok and r.json.message=='hello' and r.body=='');return 'ok'
end))
ok(server:get('/write',function()
  local count=0
  local r=curl.perform{url=upstream..'/json',write=function(bytes) count=count+#bytes;return true end}
  assert(r.ok and r.streamed and r.body=='' and count>0);return 'ok'
end))
ok(server:get('/read',function()
  local at=0
  local r=curl.perform{url=upstream..'/echo',method='POST',read=function(capacity)
    assert(capacity>0 and capacity<=65536);at=at+1
    if at==1 then return 'one' elseif at==2 then return 'two' end
  end}
  assert(r.ok and r.body=='read' and at==3);return 'ok'
end))
ok(server:get('/failed',function()
  local r=curl.perform{url=upstream..'/reset'}
  assert(not r.ok and r.code~=0 and type(r.error)=='string');return 'ok'
end))
ok(server:get('/json-upload',function()
  local r=curl.stream_json{url=upstream..'/json-upload',method='POST',request={schema=schema,value={message='uploaded'}},response={schema=schema}}
  assert(r.ok and r.json.message=='uploaded' and r.body=='');return 'ok'
end))
ok(server:get('/callback-error',function()
  local r=curl.perform{url=upstream..'/json',write=function() error('callback-marker') end}
  assert(not r.ok and r.error:find('callback-marker',1,true));return 'ok'
end))
ok(server:get('/callback-oom',function()
  local r=curl.perform{url=upstream..'/json',write=function() fixture.fail_next();return string.rep('x',30000) end}
  assert(not r.ok);collectgarbage('collect');return 'ok'
end))
ok(server:get('/sleep',function() ok(knurl.sleep(0));ok(knurl.sleep(1));return 'ok' end))
ok(server:get('/after-sleep',function(req) assert(req.phase==1);req.phase=2;ok(knurl.sleep(0));return {body='body'} end,
  {before=function(req) ok(knurl.sleep(0));req.phase=1 end,
   after=function(req,reply) assert(req.phase==2);ok(knurl.sleep(0));reply.body='ok' end}))
ok(server:get('/waiting',function() ok(knurl.sleep(80));return 'late' end))
ok(server:get('/cancel',function() fixture.curl_started();curl.perform{url=upstream..'/stalled'};continued=continued+1;return 'late' end))
ok(server:get('/timeout',function()
  local port=ok(server:listener()).port
  local r=curl.perform{url='http://127.0.0.1:'..port..'/waiting',timeout_ms=40}
  assert(not r.ok and r.code==28);return 'ok'
end))
ok(server:get('/batch-origin',function() while not released do ok(knurl.sleep(1)) end;return 'released' end))
local function self_url() return 'http://127.0.0.1:'..ok(server:listener()).port..'/batch-origin' end
ok(server:get('/batch',function() fixture.curl_batch();curl.perform{url=self_url()};continued=continued+1;return 'unexpected' end))
ok(server:get('/limit',function()
  local accepted,why=pcall(curl.perform,{url=self_url()})
  assert(not accepted and why:find('limit reached',1,true));collectgarbage('collect');return 'ok'
end))
ok(server:get('/batch-release',function() released=true;return 'ok' end))
ok(server:get('/stop',function() assert(slow==1 and cancelled==1 and continued==0 and batch_closed==16);ok(server:stop());return 'ok' end))
ok(server:run());ok(fixture.curl_join());ok(server:close())
