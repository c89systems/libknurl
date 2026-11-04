local rotmos=require('rotmos')
local json=require('lonejson').new()
local function ok(value,err) if not value then error(type(err)=='table' and (err.message..' '..err.detail) or tostring(err)) end;return value end
local text=json.schema('reply',{json.field('text',json.string{required=true})})
local problem=json.schema('problem',{json.field('code',json.string())})
local xml=json.schema('doc',{json.field('text',json.string()),json.field('@id',json.string())})
local captured=json.schema('capture',{json.field('payload',json.json_value())})
local input='{"text":"one"}'
local array_input='{"items":[{"text":"one"},{"text":"two"}]}'
local array_rows={}
local parsed_items=0
local at,done,closed=1,false,0
local array_source=ok(rotmos.source{read=function()
  if at>#array_input then done=true;return nil,rotmos.EOF end
  local part=array_input:sub(at,at+2);at=at+#part;return part
end,close=function() closed=closed+1 end})
local array_ok,array_count=rotmos.json.array_each{schema=text,source=array_source,selector='items',callback=function(row,index)
  assert(not done and index==parsed_items)
  local value,err=array_source:close();assert(value==nil and err.status==rotmos.status.STATE)
  array_rows[#array_rows+1]=row;parsed_items=parsed_items+1
  collectgarbage('collect')
end}
ok(array_ok,array_count)
assert(array_count==2 and parsed_items==2 and closed==0 and array_rows[1].text=='one' and array_rows[2].text=='two')
ok(array_source:close());assert(closed==1)
local good,err=rotmos.json.array_each{schema=text,data=array_input,selector='items',callback=function() error('array callback stopped') end}
assert(good==nil and err.status==rotmos.status.STATE and err.message:find('array callback stopped',1,true))
good,err=rotmos.json.array_each{schema=text,data='[{"text":"broken"',callback=function() error('partial item delivered') end}
assert(good==nil and err.status==rotmos.status.INVALID)
good,err=rotmos.json.array_each{schema=text,data='[{}]',callback=function() error('invalid item delivered') end}
assert(good==nil and err.status==rotmos.status.INVALID)
local callbacks=0
good,err=rotmos.json.array_each{schema=text,data=array_input,selector='items',callback=function()
  callbacks=callbacks+1;return nil,{status=rotmos.status.IO,message='item failure'}
end}
assert(good==nil and err.status==rotmos.status.IO and err.message=='item failure' and callbacks==1)
local awaiting=ok(rotmos.source{read=function() return nil,rotmos.AGAIN end})
good,err=rotmos.json.array_each{schema=text,source=awaiting,callback=function() error('unready item') end}
assert(good==nil and err.status==rotmos.status.STATE);ok(awaiting:close())
assert(ok(rotmos.json.parse{schema=text,data=input}).text=='one')
local parsed=ok(rotmos.json.parse_record{schema=text,json=input})
assert(parsed.text=='one' and ok(rotmos.json.serialize{schema=text,record=parsed})==input)
assert(ok(rotmos.xml.parse{schema=xml,xml='<doc id="7">hello</doc>',attribute_prefix='@'}).text=='hello')
assert(ok(rotmos.xml.parse_record{schema=xml,data='<doc id="7">hello</doc>',attribute_prefix='@'})['@id']=='7')
assert(ok(rotmos.xml.parse{schema=xml,data='<doc><unknown>x</unknown></doc>',skip_unknown=false,strict_unknown=false}))
for _,codec in ipairs{rotmos.json,rotmos.xml} do
  for _,config in ipairs{{schema=text},{schema=text,data='',path='anything'},{schema=text,path='bad\0path'},{schema=text,data='bad'},{schema=text,path=''}} do
    local value,err=codec.parse_record(config);assert(value==nil and err.status)
  end
  local value,err=codec.parse_record{schema=text,path=fixture.workspace..'/missing'}
  assert(value==nil and err.status==rotmos.status.IO)
end
local f=assert(io.open(fixture.workspace..'/input.json','wb'));f:write(input);f:close()
assert(ok(rotmos.json.parse_record{schema=text,path=fixture.workspace..'/input.json'}).text=='one')
f=assert(io.open(fixture.workspace..'/input.xml','wb'));f:write('<doc id="9">file</doc>');f:close()
assert(ok(rotmos.xml.parse_record{schema=xml,path=fixture.workspace..'/input.xml',attribute_prefix='@'}).text=='file')
local cr=ok(rotmos.json.parse_record{schema=captured,data='{"payload":{"n":1}}'})
assert(ok(rotmos.json.serialize{schema=captured,record=cr})=='{"payload":{"n":1}}')
for n=0,40,4 do
  fixture.fail_call(n,function() return rotmos.json.parse_record{schema=text,path=fixture.workspace..'/input.json'} end)
  fixture.fail_call(n,function() return rotmos.xml.parse_record{schema=xml,path=fixture.workspace..'/input.xml',attribute_prefix='@'} end)
  fixture.fail_call(n,function() return rotmos.json.array_each{schema=text,data=array_input,selector='items',callback=function(row) assert(row.text) end} end)
  collectgarbage('collect')
end
local big_runtime=require('lonejson').new{spool_default={memory_limit=8,max_bytes=1048576,temp_dir=fixture.workspace}}
local big=big_runtime.schema('large',{big_runtime.field('text',big_runtime.spooled_text())})
local server,retained,request,retained_output,retained_xml
local calls,before,after=0,0,0
server=ok(rotmos.server{port=0,workers={count=1,affinity_disabled=true},http={backend_quiet=true},
  limits={body_spool_directory=fixture.workspace},io={chunk_bytes=31},
  on_parent_ready=function(s) ok(fixture.spawn_typed(ok(s:listener()).port)) end,
  on_worker_exit=function() assert(calls==1 and before==1 and after==2) end})
for _,kind in ipairs{'json_route','json_typed_route','xml_route'} do
  local good,err=server[kind](server,{path='/invalid',input_schema={},handler=function() end})
  assert(good==nil and err.status==rotmos.status.INVALID)
end
local good,err=server:json_route{path='/invalid-body',input_schema=text,body=false,handler=function() end}
assert(good==nil and err.status==rotmos.status.INVALID)
good,err=server:xml_route{path='/invalid-xml',xml={typo=true},handler=function() end}
assert(good==nil and err.status==rotmos.status.INVALID)
good,err=server:json_route{path='/invalid-output-bound',max_output_bytes=-1,handler=function() end}
assert(good==nil and err.status==rotmos.status.INVALID)
for n=0,24,4 do
  local owner=ok(rotmos.server{port=0})
  fixture.fail_call(n,function() return owner:json_route{path='/allocation',input_schema=text,output_schema=text,handler=function() end} end)
  assert(owner:route_count()<=1);ok(owner:close())
end
local declaration={path='/fixed',input_schema=text,output_schema=text,handler=function(_,input,output)
  calls=calls+1;assert(type(input)=='userdata' and type(output)=='userdata')
  text:assign(output,{text=input.text})
  retained_output=output
end,after=function(_,reply)
  after=after+1
  local snapshot=ok(reply:snapshot());assert(snapshot.kind=='bytes' and snapshot.body=='{"text":"one"}')
  ok(reply:replace{status=202,body=snapshot.body,content_type=snapshot.content_type})
end}
ok(server:json_route(declaration));declaration.input_schema=nil;declaration.output_schema=nil;declaration.handler=function() error('declaration mutated') end
local group=ok(server:group('/group',{before=function() before=before+1 end,after=function() after=after+1 end}))
ok(group:json_typed_route{path='/typed',input_schema=text,handler=function(_,input)
  return {status=409,json={schema=problem,value={code=input.text}}}
end})
ok(server:json_typed_route{path='/echo',input_schema=text,handler=function(req,input)
  retained,request=input,req
  assert(ok(req.json_record(text)).text==input.text)
  local reader=ok(req.body_reader());assert(ok(reader:read(100))=='{"text":"direct"}');ok(reader:close())
  return {json_materialized={schema=text,record=input},content_type='application/vnd.rotmos+json'}
end})
ok(server:post('/array',function(req)
  local rows={}
  local success,count=req.json_array_each{schema=text,selector='items',callback=function(row,index)
    assert(index==#rows);rows[#rows+1]=row
  end}
  ok(success,count);assert(count==2 and rows[1].text=='one' and rows[2].text=='two')
  local reader=ok(req.body_reader());assert(ok(reader:read(100))==array_input);ok(reader:close())
  return 'array'
end,{body={mode='materialized',memory_buffer_limit_bytes=8}}))
ok(server:get('/retained',function()
  collectgarbage('collect');assert(retained.text=='direct' and retained_output.text=='one')
  local value,err=request.json_record(text);assert(value==nil and err.status==rotmos.status.STATE)
  value,err=request.xml_record{schema=xml};assert(value==nil and err.status==rotmos.status.STATE)
  return 'ok'
end))
local xml_options={root_element='doc',attribute_prefix='@'}
ok(server:xml_route{path='/xml',input_schema=xml,xml=xml_options,buffer_bytes=8,handler=function(req,input)
  assert(input['@id']=='7');ok(server:now())
  local bytes,err=req.body_bytes();assert(bytes==nil and err.status==rotmos.status.STATE)
  retained_xml=input
  return {xml_materialized={schema=xml,record=input,attribute_prefix='@'}}
end});xml_options.attribute_prefix='wrong'
ok(server:xml_route{path='/xml-live',input_schema=xml,xml={attribute_prefix='@'},handler=function(_,input)
  collectgarbage('collect');assert(retained_xml.text=='xml' and retained_xml['@id']=='7')
  return {xml={schema=xml,value={text=input.text,['@id']=input['@id']},attribute_prefix='@'}}
end})
ok(server:json_typed_route{path='/malformed',input_schema=text,handler=function() error('malformed handler called') end})
ok(server:json_typed_route{path='/denied',input_schema=text,auth={kind='callback',callback=function() return {action='deny'} end},handler=function() error('denied handler called') end})
ok(server:json_typed_route{path='/blocked',input_schema=text,before=function() return {status=403,body='blocked'} end,handler=function() error('blocked handler called') end})
ok(server:json_route{path='/empty',body=false,handler=function(_,input,output) assert(input==nil and output==nil) end})
ok(server:json_typed_route{path='/captured',input_schema=captured,handler=function(_,input) return {json_materialized={schema=captured,record=input}} end})
ok(server:json_typed_route{path='/large',input_schema=big,body={memory_buffer_limit_bytes=8},handler=function(req,input)
  assert(req.body_spooled and input.text:spilled())
  local source=ok(rotmos.json.source{schema=big,value={text=input.text}})
  ok(server:defer(function() input:clear();collectgarbage('collect') end))
  return {stream_source=source,content_type='application/json'}
end})
ok(server:get('/discard',function() return {json=setmetatable({},{__index=function() error('unselected getter') end})} end,
  {after=function() return {body='discarded'} end}))
ok(server:get('/bad-codec',function() return {json={schema=text,record=retained}} end))
ok(server:get('/stop',function() ok(server:stop());return 'ok' end))
ok(server:run());ok(fixture.join());ok(server:close());collectgarbage('collect')
