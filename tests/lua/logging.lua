local rotmos=require('rotmos')
local pslog=require('pslog')
local function ok(value,err) if not value then error(err and err.message or 'operation failed') end;return value end
local function invalid(value,err) assert(value==nil and err.status==rotmos.status.INVALID) end
local function read(path)
  local file=assert(io.open(path,'rb'));local value=file:read('a');file:close();return value
end
local function logger(path)
  return pslog.new_json(path,{min_level='trace',disable_timestamp=true,no_color=true}):with('service','host')
end
local callback_calls=0
local callback_logger=pslog.new_json{output=function() callback_calls=callback_calls+1 end}
for _,policy in ipairs{7,{typo=true},{success_level='unknown'}, {status_levels={[99]='info'}},
  {status_levels={[202]='info',['202']='warn'}}, {logger=callback_logger}} do
  invalid(rotmos.server{port=0,access_log=policy})
end
invalid(rotmos.server{port=0,logger=callback_logger})
local closed=logger(fixture.workspace..'/closed.log');closed:close()
invalid(rotmos.server{port=0,logger=closed})
local too_many={};for code=200,232 do too_many[code]='info' end
invalid(rotmos.server{port=0,access_log={status_levels=too_many}})
local defaults=ok(rotmos.server{port=0,http={access_log_max_bytes=0,access_log_file_count=0}})
local effective=ok(defaults:effective_config()).http
assert(tostring(effective.access_log_max_bytes)=='10485760' and effective.access_log_file_count==3
  and effective.access_log_rotation_disabled==false)
ok(defaults:close())
local coroutine_owner=coroutine.create(function()
  return ok(rotmos.server{port=0,logger=logger(fixture.workspace..'/coroutine.log')})
end)
local resumed,coroutine_server=coroutine.resume(coroutine_owner);assert(resumed)
coroutine_owner=nil;collectgarbage('collect');ok(coroutine_server:close())
for mode=0,5 do
  local root_path=fixture.workspace..'/root'..mode..'.log'
  local access_path=fixture.workspace..'/access'..mode..'.log'
  local legacy_path=fixture.workspace..'/legacy'..mode..'.log'
  local root=logger(root_path)
  local access=mode==1 and logger(access_path):with('sub','custom') or nil
  local retained=setmetatable({root,access},{__mode='v'})
  local policy={success_level='trace',client_error_level='warning',server_error_level='fatal',status_levels={['202']='info'}}
  if mode==1 then policy.logger=access
  elseif mode==2 or mode>=4 then policy=false
  elseif mode==3 then policy={enabled=false} end
  local server=ok(rotmos.server{port=0,workers={count=1,affinity_disabled=true},
    http={backend_quiet=true,access_log_path=legacy_path,
      access_log_max_bytes=mode>=4 and 1 or 10485760,
      access_log_file_count=3,access_log_rotation_disabled=mode==5},logger=root,access_log=policy,
    on_parent_ready=function(s)
      local value,err=s:set_pslog(nil);assert(value==nil and err.status==rotmos.status.STATE)
      ok(fixture.spawn_logging(ok(s:listener()).port))
    end})
  if mode==0 then
    invalid(server:set_pslog(root,{logger=callback_logger}))
    for n=0,18,3 do fixture.fail_call(n,function() return server:set_pslog(root,policy) end) end
    ok(server:set_pslog(root,policy))
    policy.status_levels['202']='panic'
  elseif mode==1 then ok(server:set_pslog(root,policy)) end
  root,access,policy=nil,nil,nil
  collectgarbage('collect');assert(retained[1] and (mode~=1 or retained[2]))
  ok(server:get('/ok',function() return {status=202,body='ok'} end))
  ok(server:get('/bad',function() return {status=404,body='missing'} end))
  ok(server:get('/fault',function() return {status=500,body='failed'} end))
  ok(server:get('/stop',function() ok(server:stop());return 'stopped' end))
  ok(server:run());ok(fixture.join());ok(server:close())
  collectgarbage('collect');assert(retained[1]==nil and retained[2]==nil)
  local structured=read(mode==1 and access_path or root_path)
  if mode<2 then
    assert(structured:find('"service":"host"',1,true))
    assert(structured:find('"sub":"'..(mode==1 and 'custom' or 'http')..'"',1,true))
    local count=0
    for line in structured:gmatch('[^\n]+') do
      if line:find('"path":',1,true) then
        count=count+1
        if line:find('"path":"/ok"',1,true) then assert(line:find('"lvl":"info"',1,true))
        elseif line:find('"path":"/bad"',1,true) then assert(line:find('"lvl":"warn"',1,true))
        elseif line:find('"path":"/fault"',1,true) then assert(line:find('"lvl":"fatal"',1,true)) end
      end
    end
    assert(count==4)
    if mode==1 then assert(not read(root_path):find('"path":',1,true)) end
  else assert(not structured:find('"path":',1,true)) end
  local legacy=read(legacy_path)
  if mode==4 then
    local one,two=read(legacy_path..'.1'),read(legacy_path..'.2')
    assert(legacy:find('/stop',1,true) and one:find('/fault',1,true) and two:find('/bad',1,true))
    assert(not (legacy..one..two):find('/ok',1,true))
    assert(io.open(legacy_path..'.3','rb')==nil)
  else
    for _,path in ipairs{'/ok','/bad','/fault','/stop'} do assert(legacy:find(path,1,true)) end
    if mode==5 then assert(io.open(legacy_path..'.1','rb')==nil) end
  end
end
for _,http in ipairs{{access_log_max_bytes=-1},{access_log_file_count=-1},
  {access_log_rotation_disabled=1},{access_log_max_bytes=rotmos.count('18446744073709551615')}} do
  invalid(rotmos.server{port=0,http=http})
end
assert(callback_calls==0);callback_logger:close()
collectgarbage('collect')
