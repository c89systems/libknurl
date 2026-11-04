local rotmos=require('rotmos')
local json=require('lonejson').new()
local function ok(value,err) if not value then error(type(err)=='table' and (err.message..' '..err.detail) or tostring(err)) end;return value end
local text=json.schema('reply',{json.field('text',json.string{required=true})})
local problem=json.schema('problem',{json.field('code',json.string())})
local xml=json.schema('doc',{json.field('text',json.string()),json.field('@id',json.string())})
local captured=json.schema('capture',{json.field('payload',json.json_value())})
local input='{"text":"one"}'
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
  collectgarbage('collect')
end
local big_runtime=require('lonejson').new{spool_default={memory_limit=8,max_bytes=1048576,temp_dir=fixture.workspace}}
local big=big_runtime.schema('large',{big_runtime.field('text',big_runtime.spooled_text())})
local server,retained,request
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
end,after=function(_,reply) after=after+1;assert(reply.json_materialized.record.text=='one');reply.status=202 end}
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
ok(server:get('/retained',function()
  collectgarbage('collect');assert(retained.text=='direct')
  local value,err=request.json_record(text);assert(value==nil and err.status==rotmos.status.STATE)
  value,err=request.xml_record{schema=xml};assert(value==nil and err.status==rotmos.status.STATE)
  return 'ok'
end))
local xml_options={root_element='doc',attribute_prefix='@'}
ok(server:xml_route{path='/xml',input_schema=xml,xml=xml_options,handler=function(_,input)
  assert(input['@id']=='7');return {xml_materialized={schema=xml,record=input,attribute_prefix='@'}}
end});xml_options.attribute_prefix='wrong'
ok(server:xml_route{path='/xml-live',input_schema=xml,xml={attribute_prefix='@'},handler=function(_,input)
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
  {after=function() return 'discarded' end}))
ok(server:get('/bad-codec',function() return {json={schema=text,record=retained}} end))
ok(server:get('/stop',function() ok(server:stop());return 'ok' end))
ok(server:run());ok(fixture.join());ok(server:close());collectgarbage('collect')
