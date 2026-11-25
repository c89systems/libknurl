-- MIT License; adapted from Vectis, Copyright (c) 2026 C89 Systems AB.
local core = require('curl.core')
local lonejson = require('lonejson')
local curl = {core = core, version = core.version, perform = core.perform}

local function copy(source)
  local value = {}
  for key, item in pairs(source or {}) do value[key] = item end
  return value
end

local function default_header(headers, name, value)
  for key in pairs(headers) do
    if type(key) == 'string' and key:lower() == name then return end
  end
  headers[name] = value
end

local function json_options(options)
  options = copy(options)
  options.headers = copy(options.headers)
  default_header(options.headers, 'content-type', 'application/json')
  default_header(options.headers, 'accept', 'application/json')
  return options
end

function curl.json(options)
  options = json_options(options)
  if options.body_json ~= nil then
    options.body = lonejson.encode_value(options.body_json)
    options.body_json = nil
  elseif options.json ~= nil then
    options.body = lonejson.encode_value(options.json)
    options.json = nil
  end

  local result = curl.perform(options)
  if result.ok and result.body ~= '' then
    local ok, value = pcall(lonejson.decode_value, result.body)
    if ok then
      result.json = value
      result.response_json = value
    else
      result.ok = false
      result.json_error = tostring(value)
      result.error = result.json_error
    end
  end
  return result
end

function curl.stream_json(options)
  options = json_options(options)
  if options.request ~= nil then
    options.request_schema = options.request.schema
    options.body_json = options.request.value
    options.request = nil
  end
  if options.response ~= nil then
    options.response_schema = options.response.schema
    options.response = nil
  end
  return curl.perform(options)
end

return curl
