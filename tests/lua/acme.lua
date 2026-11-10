local knurl=require('knurl')
local ca_fixture=require('knurl_acme_fixture')
local function ok(value,err) assert(value,err and err.message);return value end
for _,mode in ipairs{'success','returned','throw','oom'} do
  local root=fixture.workspace..'/'..mode
  fixture.private_directory(root)
  fixture.private_directory(root..'/runtime')
  local ca=ca_fixture.new(root)
  ca:start()
  local reader,writer=fixture.fd_pair()
  local notes,restored,started,usable,timed_out='',0,0,false,false
  local server
  local config={workers={count=1,death_policy='terminate',affinity_disabled=true,shutdown_timeout_ms=500},
    http={backend_quiet=true},listeners={{kind='https',port=0},{kind='https',port=0}},
    tls={identities={{mode='acme',domains={'localhost'}}},acme={email='fixture@example.test',
      directory_url=ca:directory(),runtime_directory=root..'/runtime',ca_file=root..'/ca.pem',
      persist_timeout_ms=1000,state=function(s,e)
        assert(s==server and e.runtime_directory==root..'/runtime' and e.process.process_id==ca:pid())
        local value,err=s:close();assert(value==nil and err.status==knurl.status.STATE)
        if e.event=='restore' then
          restored=restored+1;assert(e.process.role=='parent' and e.domain=='' and started==0)
          ca:restore(e.runtime_directory)
          return true
        end
        assert(e.process.role=='key_manager' and e.domain=='localhost')
        if e.event=='error' then
          local wanted=mode=='returned' and knurl.status.CONFLICT or mode=='oom' and knurl.status.NOMEM or knurl.status.STATE
          assert(e.error.status==wanted)
          fixture.fd_write(writer,'E');error('observer must not replace persistence failure')
        end
        assert(e.event=='persist' and e.error==nil)
        local file=assert(io.open(e.runtime_directory..'/certificates/localhost/fullchain.pem','rb'))
        assert(file:read(27)=='-----BEGIN CERTIFICATE-----');assert(file:close())
        fixture.fd_write(writer,'P')
        if mode=='returned' then return nil,{status=knurl.status.CONFLICT,message='durable persistence marker'} end
        if mode=='throw' then error('thrown persistence marker') end
        if mode=='oom' then fixture.fail_next();return string.rep('x',30000) end
        return true
      end}},
    on_worker_event=function(_,e) if e.kind=='started' then started=started+1 end end,
    on_tls_identity=function(_,e)
      if e.state=='usable' and not usable then
        assert(mode=='success');usable=true;ca:client(writer)
      end
    end,
    on_parent_ready=function(s)
      ca:ports(ok(s:listener(1)).port,ok(s:listener(2)).port)
      ok(s:watch{fd=reader,interest=knurl.loop.READABLE,callback=function()
        local value=fixture.fd_read(reader)
        if value then notes=notes..value end
        assert(not notes:find('F',1,true))
        if notes:find('C',1,true) then ok(s:stop()) end
      end})
      ok(s:timer{delay_ms=2000,callback=function() timed_out=true;ok(s:stop()) end})
    end}
  server=ok(knurl.server(config))
  assert(restored==0);ok(server:validate());assert(restored==0)
  config.tls.acme.state=function() error('mutated registration') end
  ok(server:get('/',function() return 'ok' end))
  local value,err=server:run()
  assert(not timed_out,'issuance timed out: '..notes)
  local tail=fixture.fd_read(reader);if tail then notes=notes..tail end
  local report=ca:report()
  assert(restored==1 and report.issued==1 and report.orders==1 and report.challenges==1 and not report.failed)
  if mode=='success' then
    ok(value,err);assert(not value.has_failure and usable and notes=='PC');ca:join()
  else
    local wanted=mode=='returned' and knurl.status.CONFLICT or mode=='oom' and knurl.status.NOMEM or knurl.status.STATE
    assert(value==nil and err.status==wanted and not usable and notes=='PE',err and err.message)
    if mode=='returned' then assert(err.message=='durable persistence marker') end
    if mode=='throw' then assert(err.message:find('thrown persistence marker',1,true)) end
  end
  ok(server:close());ca:close();fixture.fd_close(reader);fixture.fd_close(writer)
end
