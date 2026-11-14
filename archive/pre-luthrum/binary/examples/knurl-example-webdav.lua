local knurl = require('knurl')
local pslog = require('pslog')
local function must(v,e) assert(v,e and e.message);return v end
local app = must(knurl.server{bind='0.0.0.0',port=tonumber(os.getenv('PORT') or '8080'),logger=pslog.new_json()})
-- Illustrative key ONLY: replace it and use HTTPS before exposing WebDAV.
local key = os.getenv('WEBDAV_KEY') or 'replace-this-example-key'
-- Set WEBDAV_ROOT to an absolute writable directory. Files are scoped beneath site_id.
must(app:webdav{prefix='/dav',cache_dir=os.getenv('WEBDAV_ROOT') or '/srv/knurl/dav',site_id='files',
  auth={kind='callback',callback=function(req)
    if req.authorization=='Bearer '..key then return {action='allow',principal='operator'} end
    return {action='required',www_authenticate='Bearer realm="knurl"',body='Authentication required'}
  end}})
return app
