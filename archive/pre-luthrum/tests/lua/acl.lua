local knurl=require('knurl')
local function ok(v,e) assert(v,e and e.message);return v end
local function invalid(v,e) assert(v==nil and e.status==knurl.status.INVALID) end
local declarations={policy='deny',allow={addresses={'10.0.0.0/8','2001:db8::/32'}},deny={addresses={'10.1.0.0/16'}}}
for n=0,24 do
  fixture.fail_call(n,function() return knurl.acl(declarations) end)
  collectgarbage('collect')
end
local acl=ok(knurl.acl(declarations))
local value,err=acl:check('10.2.0.1')
assert(value==nil and err.status==knurl.status.STATE)
assert(acl:refresh()==true and acl:refresh()==false)
assert(acl:check('10.2.0.1')==true)
assert(acl:check('10.1.0.1')==false)
assert(acl:check('1.1.1.1','10.2.0.1')==true)
assert(acl:check('::ffff:10.2.0.1')==true)
declarations.allow.addresses[1]='0.0.0.0/0'
assert(acl:check('1.1.1.1')==false)
invalid(acl:check('localhost'))
invalid(acl:check('1.1.1.1\0hidden'))
invalid(knurl.acl{policy='invalid'})
invalid(knurl.acl{allow={addresses={key='10.0.0.1'}}})
invalid(knurl.acl{allow={countries={'us'}}})
invalid(knurl.acl{allow={countries={'US'}}})
invalid(knurl.acl{allow={addresses={'10.0.0.0/33'}}})
invalid(knurl.acl{dns_refresh_ms=-1})
invalid(knurl.acl{dns_max_addresses=1025})
for _,bad in ipairs{
  {unexpected=true}, {allow={unexpected=true}}, {use_peer='yes'},
  {dns_timeout_ms=-1}, {dns_refresh_ms=1.5}, {country_database='a\0b'},
  {allow={addresses={'a\0b'}}}, {allow={addresses={[2]='10.0.0.1'}}},
  {allow={countries={'USA'}}}, {allow={addresses={'a-.example'}}},
} do invalid(knurl.acl(bad)) end
local defaults=ok(knurl.acl{})
assert(defaults:refresh() and defaults:check('::1'))
ok(defaults:close())
local empty=ok(knurl.acl{policy='deny'})
assert(empty:refresh() and empty:check('1.2.3.4')==false)
ok(empty:close())
local s=ok(knurl.server{port=0,client_ip={trusted_proxies={'*','10.0.0.0/8'},
  forwarded_header='x-client-chain',real_ip_header='',max_hops=5,forwarded_format='comma'}})
local config=ok(s:effective_config())
assert(config.client_ip.forwarded_header=='x-client-chain' and config.client_ip.real_ip_header=='' and config.client_ip.max_hops==5)
local function denied() error('denied handler executed') end
ok(s:get('/ordinary',denied,{acl=acl}))
ok(s:json{path='/json',acl={policy='deny'},handler=denied})
ok(s:proxy{path='/proxy',target='http://127.0.0.1:1',acl=acl})
ok(s:group{prefix='/group',acl={policy='deny'},routes={{path='/blocked',acl={policy='allow'},handler=denied}}})
local group=ok(s:group('/real',{acl={policy='deny'}}))
ok(group:get('/blocked',denied,{acl={policy='allow'}}))
invalid(s:get('/invalid',denied,{acl=false}))
ok(s:validate())
ok(acl:close())
invalid(s:get('/closed-acl',denied,{acl=acl}))
ok(s:close())
ok(acl:close())
value,err=acl:check('1.1.1.1')
assert(value==nil and err.status==knurl.status.STATE)
value,err=acl:refresh()
assert(value==nil and err.status==knurl.status.STATE)
print('Lua ACL declarations, standalone checks and ownership passed')
local providers=require('knurl_provider_fixture')
local assets,storage,owner=providers.create()
local opened=providers.stats().assets_opened
local json=require('lonejson').new()
local schema=json.schema('item',{json.field('value',json.string())})
local deny=ok(knurl.acl{policy='deny'})
local called=0
local server=ok(knurl.server{port=0,workers={count=1,affinity_disabled=true},http={backend_quiet=true},
  client_ip={trusted_proxies={'127.0.0.1'},resolve=function(req)
    if req.path=='/bad-override' then return '' end
    if req.path=='/override' then return '127.0.0.1' end
  end},
  on_parent_ready=function(s)
    ok(s:set_serving(true))
    ok(fixture.spawn_acl(ok(s:listener()).port))
  end,
  on_worker_exit=function() assert(called==3 and providers.stats().assets_opened==opened) end})
ok(server:route{path='/blocked',methods={'GET','POST'},acl=deny,
  auth={kind='callback',callback=denied},before=denied,after=denied,handler=denied})
ok(server:json_route{path='/json',method='POST',acl=deny,handler=denied})
ok(server:json_typed_route{path='/typed',method='POST',acl=deny,handler=denied})
ok(server:xml_route{path='/xml',method='POST',input_schema=schema,acl=deny,handler=denied})
ok(knurl.rest.route(server,{path='/rest',method='POST',acl=deny,handler=denied}))
ok(server:static_file{path='/static',file_path=fixture.workspace..'/static/absent',acl=deny})
ok(server:static_assets{path='/assets',provider=assets,acl=deny})
ok(server:upload{path='/upload',acl=deny,write=denied,finish=denied})
ok(server:xml_route{path='/reader',method='POST',input_schema=schema,acl=deny,handler=denied})
ok(server:upload_file{path='/upload-file',file_path=fixture.workspace..'/static/blocked-output',acl=deny})
ok(server:webdav{prefix='/dav',provider=storage,auth_required=false,acl=deny})
ok(server:proxy{path='/proxy',target='http://127.0.0.1:1',acl=deny,preflight=denied,rewrite=denied})
ok(server:websocket{path='/ws',acl=deny,message=denied})
ok(ok(server:group('/group',{acl=deny})):route{path='/open',methods={'GET','POST'},acl={policy='allow'},handler=denied})
ok(server:group{prefix='/list',acl=deny,routes={{path='open',methods={'GET','POST'},acl={policy='allow'},handler=denied}}})
ok(knurl.rest.group(server,{prefix='/rest-list',acl=deny,routes={{path='/open',method='POST',acl={policy='allow'},handler=denied}}}))
ok(server:json{path='/fixed-json',body='{}',acl=deny})
ok(server:text{path='/fixed-text',body='denied',acl=deny})
ok(server:sse{path='/sse',acl=deny,open=denied,read=denied,close=denied})
ok(server:get('/allowed',function() called=called+1;return 'allowed' end,{acl={policy='deny',allow={addresses={'localhost'}}}}))
ok(server:get('/bad-override',denied))
ok(server:get('/override',function() called=called+1;return 'overridden' end,
  {acl={policy='deny',allow={addresses={'127.0.0.1'}}}}))
ok(server:get('/stop',function() called=called+1;ok(server:stop());return 'stopped' end))
ok(deny:close())
ok(server:run());ok(fixture.join());ok(server:close())
assert(io.open(fixture.workspace..'/static/blocked-output','rb')==nil)
assert(owner~=nil)
print('Lua ACL wire checks passed')
