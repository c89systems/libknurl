local knurl = require('knurl')
local pslog = require('pslog')
local function must(v,e) assert(v,e and e.message);return v end
local app = must(knurl.server{
  bind='0.0.0.0',port=tonumber(os.getenv('PORT') or '8080'),
  logger=pslog.new_json(),
})
-- Create ./www/index.html before running. Relative paths use the working directory.
must(app:static_directory{prefix='/',dir=os.getenv('STATIC_ROOT') or './www',index=true,
  acl={policy='deny',allow={addresses={'127.0.0.0/8','::1','10.0.0.0/8','192.168.0.0/16'}}}})
return app
