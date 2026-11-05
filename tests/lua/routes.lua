local rotmos=require('rotmos')
local native=require('rotmos.native')
local function ok(value,err) assert(value,err and (err.message..' '..err.detail));return value end
local function state(value,err) assert(value==nil and err.status==rotmos.status.STATE) end
local root=fixture.workspace
local function write(path,bytes)
  local file=ok(io.open(path,'wb'));ok(file:write(bytes));ok(file:close())
end
write(root..'/static/asset','A\0B');write(root..'/static/index.html','index\n')
for n=0,28,4 do
  for _,name in ipairs{'static_file','static_directory','upload_file','webdav'} do
    local owner=ok(native.server{listeners={{port=0}}})
    local spec=name=='webdav' and {path_prefix='/allocation',cache_dir=root..'/cache',site_id='allocation',auth_required=false}
      or name=='static_directory' and {path='/allocation',root_dir=root..'/static'}
      or {path='/allocation',file_path=root..'/static/asset'}
    fixture.fail_call(n,function() return owner[name](owner,spec) end)
    assert(owner:route_count()<=1);ok(owner:close())
  end
end
collectgarbage('collect')
local retained,after_error,after_oom,group_reply,group_checks,uploads= nil,nil,nil,nil,0,0
local server=ok(rotmos.server{
  port=0,workers={count=1,affinity_disabled=true},http={backend_quiet=true},
  limits={default_body_bytes=2,hard_body_bytes=16},
  on_parent_ready=function(s) ok(fixture.spawn_routes(ok(s:listener()).port)) end,
})
local spec={path='/file',file=root..'/static/asset',cache_control='public,max-age=1',
  after=function(_,reply)
    local snapshot=ok(reply:snapshot())
    assert(snapshot.kind=='source' or snapshot.status_code==304)
    retained=reply;ok(reply:header_set('x-native','kept'));return reply
  end}
ok(server:static_file(spec));spec.file='/absent'
ok(server:static_directory{prefix='/files',dir=root..'/static',index=true})
ok(server:static_directory{prefix='/no-directory-index',dir=root..'/static'})
local group=ok(server:group('/group',{after=function(_,reply)
  group_checks=group_checks+1;state(group_reply:snapshot());assert(ok(reply:snapshot()).status_code==200);return true
end}))
ok(group:static_file{path='/file',file_path=root..'/static/asset',after=function(_,reply)
  group_reply=reply;state(retained:snapshot());ok(reply:header_add('x-group','present'))
end})
ok(server:static_file{path='/replace',file_path=root..'/static/asset',after=function(_,reply)
  assert(ok(reply:snapshot()).status_code==200);return {status=202,body='replaced'}
end})
ok(server:static_file{path='/direct',file_path=root..'/static/asset',after=function(_,reply)
  ok(reply:replace{status=201,body='direct'})
end})
ok(server:static_file{path='/after-error',file_path=root..'/static/asset',after=function(_,reply)
  after_error=reply;error('expected native after failure')
end})
ok(server:static_file{path='/after-oom',file_path=root..'/static/asset',after=function(_,reply)
  after_oom=reply;fixture.fail_next();reply:snapshot()
end})
ok(server:upload_file{path='/upload-file',file=root..'/uploaded',max_body_bytes=10,
  before=function(req) assert(req.body==nil and req.body_streaming_upload) end,
  after=function(_,reply)
    uploads=uploads+1
    local snapshot=ok(reply:snapshot());assert(snapshot.status_code==200 and snapshot.body=='3')
    local file=ok(io.open(root..'/uploaded','rb'));assert(file:read('a')=='A\0B');file:close()
  end})
ok(server:upload_file{path='/upload-limit',file=root..'/never-created',max_body_bytes=2})
local principal=string.rep('p',200)
local observed={}
ok(server:webdav{prefix='/davhard',cache_dir=root..'/cache',site_id='hard',max_file_bytes=32,auth_required=false})
ok(server:webdav{prefix='/davzero',cache_dir=root..'/cache',site_id='zero',max_file_bytes=0,auth_required=false})
ok(server:webdav{prefix='/davoverride',cache_dir=root..'/cache',site_id='override',max_file_bytes=32,max_body_bytes=1,auth_required=false})
ok(server:webdav{prefix='/dav',cache_dir=root..'/cache',site_id='fixture',max_file_bytes=3,
  before=function(req) assert(req.principal==nil);req.marker=true end,
  auth={kind='callback',callback=function(req)
    assert(req.authorization=='Bearer fixture-token' and req.purpose=='webdav' and req.resource:sub(1,1)=='/')
    observed[req.resource]=(observed[req.resource] or 0)+1
    if req.resource=='/secret' then return {action='deny',body=''} end
    if req.resource=='/challenge' then return {action='required',body='auth',www_authenticate='Basic realm="routes"'} end
    if req.resource=='/redirect' then return {action='redirect',location='/login',body=''} end
    if req.resource=='/error' then error('expected WebDAV auth failure') end
    if req.resource=='/oom' then fixture.fail_next();return string.rep('x',30000) end
    return setmetatable({},{__index=function(_,name)
      if name=='action' then return 'allow' end
      if name=='principal' then return principal end
    end})
  end},after=function(req,reply)
    assert(req.marker)
    local snapshot=ok(reply:snapshot())
    if req.path=='/dav/file' and snapshot.status_code==200 then assert(req.principal==principal) end
  end})
local invalid,why=server:webdav{prefix='/noauth',cache_dir=root..'/cache',site_id='noauth'}
assert(invalid==nil and why.status==rotmos.status.INVALID)
invalid,why=server:static_file{path='/invalid',file_path='nul\0path'}
assert(invalid==nil and why.status==rotmos.status.INVALID)
ok(server:get('/stop',function()
  assert(group_checks==1 and uploads==1 and observed['/file']>=3 and observed['/']==1)
  assert(io.open(root..'/never-created','rb')==nil)
  state(after_error:snapshot());state(after_oom:snapshot());state(retained:snapshot())
  state(after_error:replace('late'));state(retained:header_set('x-late','late'))
  ok(server:stop());return 'ok'
end))
ok(server:validate());ok(server:run());ok(fixture.join());ok(server:close())
