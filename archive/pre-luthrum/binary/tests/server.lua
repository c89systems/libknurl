local k=require('knurl')
local curl=require('async.curl')
local json=require('lonejson').new()
local log=require('pslog').new_json()
local function must(v,e) assert(v,e and e.message);return v end
local schema=json.schema('result',{json.field('message',json.string{required=true})})
local app=must(k.server{
  logger=log,http={backend_quiet=true},timeouts={tls_handshake=150},
  listeners={{bind='0.0.0.0',port=8080},{kind='https',bind='0.0.0.0',port=8443}},
  tls={identities={{domains={'localhost'},certificate={kind='file',path='/cert.pem'},private_key={kind='file',path='/key.pem'}}}},
  on_parent_stopped=function() log:info('scratch-parent-stopped') end,
  on_parent_stopping=function()
    if os.getenv('INJECT_STOP_HANG')=='1' then while true do end end
  end,
})
must(app:get('/ready',function() return 'ready' end))
must(app:get('/upstream',function() return {body='{"message":"verified"}',content_type='application/json'} end))
must(app:get('/api',function()
  local r=curl.stream_json{url='https://localhost:8443/upstream',response={schema=schema},timeout_ms=2000}
  if not r.ok then return {status=502,body=r.error or 'failed'} end
  assert(r.json.message=='verified' and r.body=='')
  return 'verified'
end))
must(app:get('/wrong-ca',function()
  local r=curl.perform{url='https://localhost:8443/upstream',ca_file='/missing-ca.pem',timeout_ms=2000}
  assert(not r.ok);return 'rejected'
end))
must(app:get('/deny',function() error('denied handler ran') end,{acl={policy='deny'}}))
must(app:static_directory{prefix='/static',dir='/www',acl={policy='allow'}})
must(app:proxy{path='^/proxy$',path_kind='regex',target='https://localhost:8443',rewrite=function(_,out) must(out:set_path('/upstream')) end})
must(app:webdav{prefix='/dav',cache_dir='/tmp',site_id='scratch',
  auth={kind='callback',callback=function(r)
    return r.authorization=='Bearer fixture-key' and {action='allow',principal='fixture'} or {action='required',www_authenticate='Bearer realm="fixture"',body='auth'}
  end}})
-- Exercise country database ownership without relying on a live downloaded DB.
must(app:get('/country',function() return 'country' end,{acl={policy='allow',country_database='/country.mmdb',deny={countries={'KP','VE'}}}}))
must(app:get('/pid1',function()
  local f=assert(io.open('/proc/1/comm','r'));local name=f:read('l');f:close()
  assert(name=='knurl');return name
end))
must(app:get('/orphan',function()
  local f=assert(io.open('/tmp/orphan.pid','r'));local pid=assert(tonumber(f:read('l')));f:close()
  f=io.open('/proc/'..pid..'/status','r')
  if f then f:close();return 'waiting' end
  return 'reaped'
end))
return app
