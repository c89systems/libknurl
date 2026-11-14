local knurl = require('knurl')
local pslog = require('pslog')
local function must(v,e) assert(v,e and e.message);return v end
local log = pslog.new_json()
local path = os.getenv('COUNTRY_MMDB') or './dbip-country-lite.mmdb'
local file = io.open(path,'rb')
if not file then
  log:error('Country MMDB missing. Download MMDB from https://db-ip.com/db/download/ip-to-country-lite; run gzip -dk dbip-country-lite-YYYY-MM.mmdb.gz; set COUNTRY_MMDB to the extracted .mmdb path. Country data is CC BY 4.0: include attribution and a link to https://db-ip.com/.', 'path',path)
  error('country ACL requires readable MMDB: '..path)
end
file:close()
local countries = must(knurl.acl{policy='deny',country_database=path,
  allow={countries={'SE','NO','FI','GB','DE','US','CL'}},
  deny={countries={'KP','VE'},unknown=true}})
-- GB is the UK; KP is North Korea. Unknown countries fail closed.
local app = must(knurl.server{bind='0.0.0.0',port=tonumber(os.getenv('PORT') or '8080'),logger=log,
  -- Trust only your actual proxies; never trust arbitrary public senders.
  client_ip={trusted_proxies={'127.0.0.1'},forwarded_header='x-forwarded-for'}})
must(app:static_directory{prefix='/public',dir=os.getenv('STATIC_ROOT') or './www',index=true,acl=countries})
must(app:proxy{path='^/api(/.*)?$',path_kind='regex',target=os.getenv('UPSTREAM') or 'http://127.0.0.1:9000',acl=countries})
must(app:get('/health',function() return 'ok\n' end,
  {acl={policy='deny',allow={addresses={'127.0.0.0/8','::1','10.0.0.0/8'}}}}))
return app
