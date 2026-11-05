local rotmos=require('rotmos')
local lonejson=require('lonejson')
local rest=rotmos.rest
local function ok(value,err) if not value then error(type(err)=='table' and err.message or tostring(err)) end;return value end
local function failed(value,err,status) assert(value==nil and err.status==status,err and err.message) end
assert(ok(rest.json(nil)).body=='null\n')
assert(ok(rest.json({a=1})).body=='{"a":1}\n')
assert(ok(rest.json({1,2},{newline=false,status_code=201})).body=='[1,2]')
assert(ok(rest.json({},{max_bytes=3})).body=='{}\n')
-- A buffered REST reply forwards the encoder's bytes unchanged, plus newline.
-- Revalidating/normalizing through a second JSON writer changes this result.
local encode=lonejson.encode_json_to_sink
local encoding_calls=0
lonejson.encode_json_to_sink=function(value,sink)
  assert(value.marker);encoding_calls=encoding_calls+1
  sink('{ "n" : 1e+02, "s" : "\\u0061" }')
end
assert(ok(rest.json({marker=true})).body=='{ "n" : 1e+02, "s" : "\\u0061" }\n')
assert(encoding_calls==1)
lonejson.encode_json_to_sink=encode
-- Explicit argument order keeps the helper's second return beside its value.
local function json_failure(value,opts,status)
  local result,err=rest.json(value,opts);failed(result,err,status);assert(err.kind=='json_encode')
end
json_failure({}, {max_bytes=2},rotmos.status.LIMIT)
json_failure(string.rep('x',65536),{max_bytes=32},rotmos.status.LIMIT)
local cycle={};cycle.self=cycle
for _,value in ipairs{cycle,{[2]='hole'}, {1,a=2}, {bad=function() end}, 0/0, math.huge} do
  json_failure(value,nil,rotmos.status.INVALID)
end
for _,opts in ipairs{{max_bytes=-1},{newline=1},{status=99},{status_code=600},{content_type='x\0y'},{typo=true}} do
  json_failure({},opts,rotmos.status.INVALID)
end
local err={kind='custom',message='bad',status=rotmos.status.CONFLICT,extra={a=1}}
local response=rest.error_response(err)
assert(response.status==409 and lonejson.decode_json(response.body).error.extra.a==1 and err.source_code==nil)
assert(rest.error_response({http_status=422}).status==422)
assert(rest.error_response({status=rotmos.status.NOT_SUPPORTED}).status==501)
assert(rest.error_response({status=rotmos.status.TIMEOUT}).status==504)
assert(rest.error_response(cycle).status==500)
for n=0,40,4 do
  fixture.fail_call(n,function() return rest.json({a={1,2},text=string.rep('x',4097)}) end)
end
local json=lonejson.new()
local schema=json.schema('Item',{json.field('n',json.string{required=true})})
local owner=ok(rotmos.server{port=0})
local docs={path='/items/:id?',methods={'GET','POST'},summary='items',operationId='items',tags={'test'},
  request={name='Item',schema=schema},responses={{status_code=200,desc='ok',schema_name='Item',schema=schema},{status=204}}}
ok(owner:openapi_doc(docs));assert(owner:route_count()==0)
docs.summary='mutated';docs.tags[1]='mutated';docs.responses[1].desc='mutated'
local document=lonejson.decode_json(ok(owner:openapi{title='test',version='7'}))
assert(document.openapi=='3.1.0' and document.info.title=='test' and document.info.version=='7')
assert(document.paths['/items/{id}'].get.summary=='items')
assert(document.paths['/items/{id}'].post.responses['200'].description=='ok')
assert(document.paths['/items/{id}'].post.responses['204'].content==nil)
assert(document.components.schemas.Item.required[1]=='n')
local yaml=ok(owner:openapi{format='yml'});assert(yaml:find('openapi: 3.1.0',1,true))
local value,problem=owner:openapi_doc(docs);failed(value,problem,rotmos.status.CONFLICT)
for _,opts in ipairs{{max_bytes=1},{max_bytes=-1},{format='xml'},{title='bad\0title'}} do
  value,problem=owner:openapi(opts);assert(value==nil and problem.status)
end
for _,opts in ipairs{{path='/bad',request={schema=schema}}, {path='/bad',responses={}},
  {path='/bad',tags={1}}, {path='/bad',responses={{status=200,schema={}}}},
  {path='/bad',responses={{status=200},{status=200}}}} do
  value,problem=owner:openapi_doc(opts);assert(value==nil and problem.status)
end
local weak=setmetatable({schema},{__mode='v'})
schema=nil;docs=nil;collectgarbage('collect');assert(weak[1]~=nil)
ok(owner:close());collectgarbage('collect');assert(weak[1]==nil)
value,problem=owner:openapi();failed(value,problem,rotmos.status.STATE)
for n=0,48,4 do
  local s=ok(rotmos.server{port=0})
  local sample=json.schema('Allocation'..n,{json.field('text',json.string())})
  local retained=setmetatable({sample},{__mode='v'})
  fixture.fail_call(n,function() return s:openapi_doc{path='/oom',tags={'one','two'},
    request={name='Allocation',schema=sample},responses={{status=200,description='ok'}}} end)
  fixture.fail_call(n,function() return s:openapi() end)
  ok(s:close());sample=nil;collectgarbage('collect');assert(retained[1]==nil)
end
local server,calls,closed,before,after,errors
local validation_closed,policy_after,parse_errors=0,0,0
local decoded,parse_error=pcall(lonejson.decode_json,'{')
assert(not decoded)
calls,closed,before,after,errors=0,0,0,0,0
local marker={private='original Lua error'}
server=ok(rotmos.server{port=0,workers=1,on_parent_ready=function(s)
  ok(fixture.spawn_rest(ok(s:listener(1)).port))
end,on_worker_exit=function()
  assert(calls==1 and closed==1 and before==1 and after==1 and errors==1)
  assert(validation_closed==1 and policy_after==0 and parse_errors==1)
end})
for _,body in ipairs{'stream','materialized','streaming_upload'} do
  value,problem=rest.route(server,{path='/bad',body=body,handler=function() end})
  failed(value,problem,rotmos.status.INVALID)
end
for _,name in ipairs{'decode','decode_disabled','on_error'} do
  local count=server:route_count()
  local result,why=rest.route(server,{path='/reserved',handler=function() end,[name]=false})
  failed(result,why,rotmos.status.INVALID);assert(server:route_count()==count)
end
ok(rest.route(server,{path='/echo',method='POST',openapi={summary='echo'},handler=function(req)
  calls=calls+1;return {n=req.json.n},{status=202}
end,after=function(req,reply)
  if req.body=='{' then
    local cause=lonejson.decode_json(reply.body).error
    assert(cause.kind=='json_parse' and cause.status==rotmos.status.INVALID and
      cause.source_code==require('rotmos.native').rest.ERROR_SOURCE_LONEJSON and cause.message==tostring(parse_error),reply.body)
    parse_errors=parse_errors+1
  end
end}))
ok(rest.route(server,{path='/validate',method='POST',validate=function() return false end,handler=function() error('validation bypassed') end}))
ok(rest.route(server,{path='/validate-reply',method='POST',validate=function() return {status=422,body='validation'} end,handler=function() error('validation bypassed') end}))
ok(rest.route(server,{path='/validate-throw',method='POST',validate=function() error('validation') end,handler=function() error('validation bypassed') end}))
ok(rest.route(server,{path='/validate-live',validate=function()
  local read=false
  return {status=203,stream_source={read=function() if not read then read=true;return 'validated live' end end,
    close=function() validation_closed=validation_closed+1 end}}
end,handler=function() error('validation bypassed') end,after=function(_,reply)
  assert(type(reply)=='table' and reply.stream_source)
end}))
for _,case in ipairs{{path='/rewritten',value='{"edited":true}'},{path='/injected',value='{"edited":true}'}} do
  ok(rest.route(server,{path=case.path,method='POST',before=function(req) req.body=case.value end,
    validate=function(value) assert(value.edited==true) end,handler=function(req) return req.json end}))
end
ok(rest.route(server,{path='/removed',method='POST',before=function(req) req.body=nil end,
  handler=function(req) assert(req.json==nil);return {body='skipped'} end}))
ok(rest.route(server,{path='/policy-fail',handler=function() error(marker) end,
  error_handler=function(err) assert(err==marker);error('error policy failed') end,
  after=function() policy_after=policy_after+1 end}))
ok(rest.route(server,{path='/throw',handler=function() error('handler') end}))
ok(rest.route(server,{path='/recover',handler=function() error(marker) end,error_handler=function(err,req)
  assert(err==marker and req.path=='/recover');errors=errors+1;return {status=418,body='recovered'}
end}))
ok(rest.route(server,{path='/raw',method='POST',decode_json=false,body=true,handler=function(req) assert(req.json==nil);return {status=201,body=req.body} end}))
ok(rest.route(server,{path='/empty',handler=function(req) assert(req.json==nil) end}))
ok(rest.route(server,{path='/null',method='POST',handler=function(req) return req.json==lonejson.json_null end}))
ok(rest.route(server,{path='/source',handler=function()
  local read=false
  return {stream_source={read=function() if not read then read=true;return 'live' end end,close=function() closed=closed+1 end}}
end}))
ok(rest.group(server,{path_prefix='/api',method='POST',response={status=201,newline=false},
  before=function() before=before+1 end,after=function(_,reply) after=after+1;assert(reply.body=='{"n":1}') end,
  routes={{path='/one',handler=function(req) return req.json end}}}))
local group=ok(server:group('/native/'))
ok(rest.route(group,{path='/r',method='POST',openapi={summary='native'},handler=function() return {} end}))
local regex_group=ok(server:group('/v1.0'))
ok(regex_group:route{path='^/items/[0-9]+$',openapi={summary='regex'},handler=function() end})
ok(server:json_typed_route{path='/typed-doc',body=false,openapi={summary='typed'},handler=function() end})
ok(server:get('/undocumented',function() end,{openapi=false}))
local fixed={path='/fixed-text',body='fixed\0discarded',status_code='0',status='201',cache_control='no-store',openapi={summary='fixed'}}
ok(server:text(fixed));fixed.body='mutated';fixed.cache_control='private'
ok(server:text{path='/fixed-default'})
ok(server:json{path='/fixed-json'})
ok(server:json{path='/fixed-json-raw',body=' not JSON ',content_type='application/vnd.fixture+json'})
ok(server:redirect{path='/fixed-redirect',location='/fixed-text',status=303})
ok(server:auth_json{path='/fixed-auth',auth={kind='callback',callback=function() return {action='deny'} end}})
local fixed_group=ok(server:group('/fixed'))
ok(fixed_group:text{path='/group',body='group'})
for _,case in ipairs{{'text',{path='/bad-text',status=99}}, {'redirect',{path='/bad-redirect',location='/x',status=200}},
  {'redirect',{path='/bad-redirect'}}, {'auth_json',{path='/bad-auth'}}} do
  local count=server:route_count()
  local value,why=server[case[1]](server,case[2]);failed(value,why,rotmos.status.INVALID)
  assert(server:route_count()==count)
end
local count=server:route_count()
value,problem=server:route{path='/invalid-doc',openapi=true,handler=function() end}
failed(value,problem,rotmos.status.INVALID);assert(server:route_count()==count)
value,problem=server:route{path='/annotation-fails',openapi={responses={}},handler=function() end}
failed(value,problem,rotmos.status.INVALID);assert(server:route_count()==count+1)
ok(rest.route(server,{path='/bounded',response={max_bytes=2},handler=function() return {} end}))
ok(rest.route(server,{path='/blocked',method='POST',before=function() return {status=401,body='blocked'} end,handler=function() error('before bypassed') end}))
ok(rest.route(server,{path='/denied',method='POST',auth={kind='callback',callback=function() return {action='deny'} end},handler=function() error('auth bypassed') end}))
ok(rest.route(server,{path='/limited',method='POST',max_body_bytes=4,handler=function() error('body limit bypassed') end}))
ok(server:get('/docs',function()
  local doc=lonejson.decode_json(ok(server:openapi()))
  assert(doc.paths['/native/r'].post.summary=='native' and doc.paths['/echo'].post.summary=='echo')
  assert(doc.paths['^/v1\\.0/items/[0-9]+$'].get.summary=='regex')
  assert(doc.paths['/typed-doc'].post.summary=='typed' and doc.paths['/undocumented']==nil)
  local a,b=server:openapi_doc{path='/during-run'};failed(a,b,rotmos.status.STATE)
  return ok(server:openapi())
end))
ok(server:get('/stop',function() ok(server:stop());return 'ok' end))
ok(server:run());ok(fixture.join());ok(server:close());collectgarbage('collect')
