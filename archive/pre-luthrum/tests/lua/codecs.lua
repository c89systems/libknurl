local knurl=require('knurl')
local json=require('lonejson').new()
local function ok(value,err) if not value then error(type(err)=='table' and (err.message..' '..err.detail) or tostring(err)) end;return value end
local schema=json.schema('writer',{json.field('text',json.string()),json.field('@id',json.string())})
local record=schema:new_record()
ok(schema:assign(record,{text='external',['@id']='0'}))
assert(ok(knurl.json.serialize{schema=schema,record=record})=='{"text":"external","@id":"0"}')
assert(ok(knurl.xml.serialize{schema=schema,record=record,attribute_prefix='@'})=='<writer id="0">external</writer>')
for _,codec in ipairs{knurl.json,knurl.xml} do
  for _,options in ipairs{{schema={},value={}},{schema=schema,value={},max_bytes=-1},{schema=schema,value={},typo=true}} do
    local value,err=codec.serialize(options)
    assert(value==nil and err.status==knurl.status.INVALID)
  end
  for _,method in ipairs{'write','source'} do
    local success,err=pcall(codec[method],{schema=schema,record=record},function() end)
    assert(not success and err:find('serialize to buffer',1,true))
  end
  local value,err=codec.serialize{schema=schema,value={text='too large'},max_bytes=1}
  assert(value==nil and err.status==knurl.status.LIMIT)
end
local opts={schema=schema,root_element='initial-root',attribute_prefix='@',value={text='hello<&',['@id']='7'}}
local chunks,total,writes={},0,0
local success,n=knurl.xml.write(opts,function(bytes)
  writes=writes+1;total=total+#bytes;chunks[#chunks+1]=bytes
  opts.root_element='changed';opts.attribute_prefix='wrong';opts.value.text='mutated'
  record:clear();schema:assign(record,{text='unrelated'})
  collectgarbage('collect')
  assert(ok(knurl.xml.serialize{schema=schema,value={text='nested'}}):find('nested',1,true))
  return true
end)
assert(success and n==total and writes>0)
assert(table.concat(chunks)=='<initial-root id="7">hello&lt;&amp;</initial-root>')
for _,codec in ipairs{knurl.json,knurl.xml} do
  for _,sink in ipairs{function() error({message='sink throw'}) end,function() return nil,{message='sink reject'} end,function() return false end} do
    local value,err=codec.write({schema=schema,value={text='tiny'}},sink)
    assert(value==nil and err.message:find('sink',1,true))
  end
  local value,err=codec.write({schema=schema,value={text='tiny'}},{write=function() error('method throw') end})
  assert(value==nil and err.message:find('method throw',1,true))
  local passed,why=pcall(codec.write,{schema=schema,value={text='tiny'}},setmetatable({},{__index=function() error('getter throw') end}))
  assert(not passed and why:find('getter throw',1,true))
  assert(ok(codec.serialize(setmetatable({},{__index={schema=schema,value={text='inherited'}}}))):find('inherited',1,true))
  local declared={schema=schema,value={text='original'}}
  local source=ok(codec.source(declared))
  local reset,why=source:reset();assert(reset==nil and why.status==knurl.status.NOT_SUPPORTED)
  declared.value.text='changed';declared.schema=nil;collectgarbage('collect')
  local out={}
  while true do
    local bytes,why=source:read(3)
    if not bytes then assert(why==knurl.EOF);break end
    assert(#bytes>0 and #bytes<=3);out[#out+1]=bytes
  end
  assert(table.concat(out):find('original',1,true))
  ok(source:close());ok(source:close())
  value,err=source:read();assert(value==nil and err.status==knurl.status.STATE)
  local cancelled=ok(codec.source{schema=schema,value={text=string.rep('z',32768)}})
  assert(#ok(cancelled:read(7))<=7);ok(cancelled:close())
  for n=0,45,3 do
    fixture.fail_call(n,function() return codec.serialize{schema=schema,value={text='allocation'}} end)
    local passed,producer=fixture.fail_call(n,function() return codec.source{schema=schema,value={text='allocation'}} end)
    if passed and producer then ok(producer:close()) end
    fixture.fail_call(n,function() return codec.write({schema=schema,value={text='allocation'}},function() fixture.fail_next() end) end)
    collectgarbage('collect')
  end
end
local runtime=require('lonejson').new{spool_default={memory_limit=8,max_bytes=1048576,temp_dir=fixture.workspace}}
local large=runtime.schema('large',{runtime.field('text',runtime.spooled_text())})
local backing=large:new_record()
ok(large:decode_into(backing,'{"text":"'..string.rep('x',65536)..'"}'))
local spool=backing.text;assert(spool:spilled())
local largest,delivered,calls=0,0,0
local sink={}
sink.write=function(bytes)
  largest=math.max(largest,#bytes);delivered=delivered+#bytes;calls=calls+1
  sink.write=function() error('sink function was not copied') end
  if calls==1 then backing:clear();spool=nil;collectgarbage('collect') end
end
ok(knurl.xml.write({schema=large,value={text=spool}},sink))
assert(largest<=4096 and delivered==65536+#'<large></large>' and calls>16)
local server
server=ok(knurl.server{port=0,workers={count=1,affinity_disabled=true},http={backend_quiet=true},
  io={chunk_bytes=7},on_parent_ready=function(s) ok(fixture.spawn_codecs(ok(s:listener()).port)) end})
for _,name in ipairs{'json','xml'} do
  local codec=knurl[name]
  ok(server:route{path='/'..name,methods={'GET','HEAD'},handler=function()
    local source=ok(codec.source{schema=schema,value={text='wire'}})
    return {stream_source=source,content_type=name=='json' and 'application/json' or 'application/xml'}
  end})
end
ok(server:get('/stop',function() ok(server:stop());return 'ok' end))
ok(server:run());ok(fixture.join());ok(server:close())
collectgarbage('collect')
