local rotmos=require('rotmos')
local module_dir=os.getenv('ROTMOS_PROVIDER_TEST_DIR')
if module_dir then package.cpath=module_dir..'/?.so;'..package.cpath end
local providers=require('rotmos_provider_fixture')
local function ok(value,err) if not value then error(err and err.message or 'operation failed') end;return value end
local function invalid(value,err) assert(value==nil and err.status==rotmos.status.INVALID) end
for n=0,18,3 do fixture.fail_call(n,providers.create);collectgarbage('collect') end
local baseline=providers.stats().owners_closed
local assets,storage,owner=providers.create()
local retained=setmetatable({owner},{__mode='v'})
local server=ok(rotmos.server{port=0,workers={count=1,affinity_disabled=true},http={backend_quiet=true},io={chunk_bytes=31},
  on_parent_ready=function(s) ok(fixture.spawn_providers(ok(s:listener()).port)) end,
  on_worker_exit=function()
    local stats=providers.stats()
    assert(stats.assets_opened==stats.assets_closed and stats.reads>100 and stats.maximum_read<=31 and stats.range_reads==1)
  end})
invalid(server:static_assets{path='/invalid'})
invalid(server:static_assets{path='/wrong',provider=storage})
invalid(server:webdav{prefix='/wrong-dav',provider=assets,auth_required=false})
invalid(server:static_directory{path='/wrong-dir',provider=assets})
local declaration={prefix='/pack',provider=assets}
ok(server:static_assets(declaration));declaration.provider=nil
ok(ok(server:group('/group')):static_assets{path='/pack',provider=assets})
ok(server:webdav{prefix='/dav',provider=storage,auth_required=false})
assets,storage,owner=nil,nil,nil
collectgarbage('collect');assert(retained[1] and providers.stats().owners_closed==baseline)
ok(server:get('/stop',function() ok(server:stop());return 'stopped' end))
ok(server:run());ok(fixture.join());ok(server:close())
collectgarbage('collect');assert(retained[1]==nil and providers.stats().owners_closed==baseline+1)
