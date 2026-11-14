local file,check=...
local k=require('knurl')
k.configure_defaults{profile='production_webserver',workers={count=1,affinity_disabled=true},
  timeouts={deferred_response={milliseconds=30000,disabled=false}}}
local app=assert(loadfile(file,'t'))()
assert(type(app)=='table' and type(app.validate)=='function' and
  type(app.run)=='function' and type(app.close)=='function',
  'configuration must return a Knurl server')
local function message(e)
  if type(e)=='table' then return e.message..' '..(e.detail or '') end
  return tostring(e)
end
local ok,e=app:validate()
if ok and not check then ok,e=app:run() end
local closed,ce=app:close()
if not ok then error(message(e),0) end
if not closed then error(message(ce),0) end
