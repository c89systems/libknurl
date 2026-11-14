-- MIT License; adapted from Vectis, Copyright (c) 2026 C89 Systems AB.
local supplied = ...
local core = type(supplied)=='table' and supplied or require('knurl.native').curl
local lonejson = require('lonejson')
local M = {core=core,version=core.version}
local function copy(source)
  local value={}
  for k,v in pairs(source or {}) do value[k]=v end
  return value
end
local function default(headers,name,value)
  for k in pairs(headers) do if type(k)=='string' and k:lower()==name then return end end
  headers[name]=value
end
function M.perform(opts) return core.perform(opts) end
function M.json(opts)
  opts=copy(opts);opts.headers=copy(opts.headers)
  default(opts.headers,'content-type','application/json');default(opts.headers,'accept','application/json')
  if opts.body_json~=nil then opts.body=lonejson.encode_value(opts.body_json);opts.body_json=nil
  elseif opts.json~=nil then opts.body=lonejson.encode_value(opts.json);opts.json=nil end
  local result=M.perform(opts)
  if result.ok and result.body and result.body~='' then
    local ok,value=pcall(lonejson.decode_value,result.body)
    if ok then result.json=value;result.response_json=value
    else result.ok=false;result.json_error=tostring(value);result.error=result.json_error end
  end
  return result
end
function M.stream_json(opts)
  opts=copy(opts);opts.headers=copy(opts.headers)
  default(opts.headers,'content-type','application/json');default(opts.headers,'accept','application/json')
  if opts.request~=nil then opts.request_schema=opts.request.schema;opts.body_json=opts.request.value;opts.request=nil end
  if opts.response~=nil then opts.response_schema=opts.response.schema;opts.response=nil end
  return M.perform(opts)
end
return M
