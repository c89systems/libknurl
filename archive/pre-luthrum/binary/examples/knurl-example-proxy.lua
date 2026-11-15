local knurl = require('knurl')
local pslog = require('pslog')
local function must(v,e) assert(v,e and e.message);return v end
local app = must(knurl.server{bind='0.0.0.0',port=tonumber(os.getenv('PORT') or '8080'),logger=pslog.new_json()})
-- HTTPS validates upstream certificates with system trust by default.
must(app:proxy{path='^/.*$',path_kind='regex',target=os.getenv('UPSTREAM') or 'http://127.0.0.1:9000',connect_timeout_ms=3000,idle_timeout_ms=15000,total_timeout_ms=30000})
return app
