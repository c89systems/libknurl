local native = require('rotmos.native')
local M = { AGAIN = native.AGAIN, EOF = native.EOF, count = native.count, status = native.status }
M.websocket,M.loop,M.sse_encode=native.websocket,native.loop,native.sse_encode
M.control=native.control
local Server, Group = {}, {}
Server.__index, Group.__index = Server, Group
local requests = setmetatable({}, {__mode='k'})
local request_ready = setmetatable({}, {__mode='k'})
local function copy(t)
  local out = {}
  for k,v in pairs(t or {}) do out[k]=v end
  return out
end
local function invalid(message)
  return nil,{status=native.status.INVALID,kind='invalid argument',source=1,native_code=0,operation='lua',message=message,detail=''}
end
local function request(handle,early)
  local out = requests[handle]
  if out then
    out.principal=handle:principal()
    if early or not request_ready[handle] then
      local fresh,err=handle:snapshot()
      if not fresh then error(err.message) end
      out.ip,out.correlation_id=fresh.ip,fresh.correlation_id
      if not early then
        for key,value in pairs(fresh) do out[key]=value end
        out.body,out.body_path=fresh.body,fresh.body_path
        request_ready[handle]=true
      end
    end
    return out
  end
  local err
  out,err=handle:snapshot()
  if not out then error(err.message) end
  out._native=handle
  out.headers=handle:headers()
  out.queries=handle:queries()
  local function lookup(name)
    return function(a,b) return handle[name](handle,b or a) end
  end
  out.header,out.query,out.param=lookup('header'),lookup('query'),lookup('param')
  out.body_bytes=function() return handle:body_bytes() end
  for _,name in ipairs{'body_reader','body_materialize','body_spill','body_copy','json_record','xml_record'} do
    out[name]=function(a,b)
      if a==out then a=b end
      return handle[name](handle,a)
    end
  end
  out.body_resume=function() return handle:body_resume() end
  out.producer_resume=function() return handle:producer_resume() end
  out.event=lookup('event')
  requests[handle]=out
  request_ready[handle]=not early
  return out
end
M.source=native.source
M.json,M.xml=native.json,native.xml
local function options(opts,handler,body_mode)
  opts=copy(opts)
  local fn=handler or opts.handler
  if type(fn)=='function' then opts.handler=function(req) return fn(request(req)) end end
  for _,name in ipairs{'before','after'} do
    local hook=opts[name]
    if type(hook)=='function' then
      opts[name]=function(req,value)
        return hook(request(req),value)
      end
    end
  end
  if type(opts.body)=='string' then opts.body={mode=opts.body}
  elseif opts.body==true then opts.body={mode='buffered'}
  elseif opts.body==false then opts.body={mode='none'}
  elseif type(opts.body)=='table' then opts.body=copy(opts.body) end
  if type(opts.body)=='table' then
    if opts.body.mode=='buffered' then opts.body.mode=native.BODY_BUFFERED
    elseif opts.body.mode=='materialized' then opts.body.mode=native.BODY_MATERIALIZED
    elseif opts.body.mode=='streaming_upload' then opts.body.mode=native.BODY_STREAMING_UPLOAD
    elseif opts.body.mode=='stream' then opts.body.mode=native.BODY_STREAM
    elseif opts.body.mode=='none' then opts.body.mode=native.BODY_NONE
    elseif opts.body.mode==nil then opts.body.mode=body_mode or native.BODY_NONE end
  end
  return opts
end
local function register(self,name,opts,default_method)
  local doc=opts.openapi
  if doc~=nil and doc~=false and type(doc)~='table' then return invalid('route openapi must be a table') end
  if type(doc)=='table' then
    doc=copy(doc)
    doc.path=opts.path or opts.path_prefix
    doc.method,doc.methods=opts.method or default_method,opts.methods
    if self._prefix and self._prefix~='' and self._prefix~='/' and type(doc.path)=='string' then
      local prefix=self._prefix
      if opts.path_kind=='regex' or doc.path:sub(1,1)=='^' then
        prefix=prefix:gsub('([%.%+%*%?%^%$%(%)%[%]{}|\\])','\\%1')
        doc.path='^'..prefix..(doc.path:sub(1,1)=='^' and doc.path:sub(2) or doc.path)
      else doc.path=prefix..doc.path end
    end
  end
  opts.openapi=nil
  local handle,err=self._native[name](self._native,opts)
  if not handle then return nil,err end
  if type(doc)=='table' then return (self._server or self):openapi_doc(doc) end
  return true
end
local function route(self,path,handler,opts)
  if type(path)=='table' then opts=path;handler=opts.handler;path=opts.path end
  if opts~=nil and type(opts)~='table' then return invalid('route options table required') end
  opts=options(opts,handler);opts.path=path
  return register(self,'route',opts,'GET')
end
Server.route,Group.route=route,route
local function typed_route(self,kind,declaration)
  if type(declaration)~='table' or type(declaration.handler)~='function' then
    return invalid(kind..' declaration and handler required')
  end
  local opts=copy(declaration)
  local handler,input_schema,output_schema=opts.handler,opts.input_schema,opts.output_schema
  local output_limit=opts.max_output_bytes
  local xml=type(opts.xml)=='table' and copy(opts.xml) or opts.xml
  opts.xml=xml
  opts=options(opts,function(req)
    local input,err
    if input_schema~=nil then
      if kind=='xml_route' then
        local config=copy(xml);config.schema=input_schema
        input,err=req.xml_record(config)
      else input,err=req.json_record(input_schema) end
      if not input then return nil,err end
    end
    if kind=='json_route' then
      local output=output_schema and output_schema:new_record() or nil
      local _,failure=handler(req,input,output)
      if failure~=nil then return nil,failure end
      if output then return {json_materialized={schema=output_schema,record=output,max_bytes=output_limit}} end
      return nil
    end
    return handler(req,input)
  end,native.BODY_MATERIALIZED)
  return register(self,kind,opts,'POST')
end
for _,name in ipairs{'json_route','json_typed_route','xml_route'} do
  Server[name]=function(self,opts) return typed_route(self,name,opts) end
  Group[name]=Server[name]
end
local function first(a,b,c)
  if a~=nil then return a end
  if b~=nil then return b end
  return c
end
local function upload(self,opts)
  if type(opts)~='table' then return invalid('upload declaration table required') end
  opts=options(opts,nil,native.BODY_STREAMING_UPLOAD)
  local callbacks={open=first(opts.open,opts.on_open),on_chunk=first(opts.on_chunk,opts.chunk,opts.write),
    on_complete=first(opts.on_complete,opts.complete,opts.finish),close=first(opts.close,opts.on_close)}
  opts.on_open,opts.chunk,opts.write,opts.complete,opts.finish,opts.on_close=nil,nil,nil,nil,nil,nil
  for name,fn in pairs(callbacks) do
    if type(fn)=='function' then opts[name]=function(req,...) return fn(request(req),...) end
    else opts[name]=fn end
  end
  return register(self,'upload',opts,'POST')
end
Server.upload,Group.upload=upload,upload
local function convenience(self,name,opts)
  if type(opts)~='table' then return invalid(name..' declaration table required') end
  opts=copy(opts)
  if name=='static_directory' then
    opts.path=first(opts.path,opts.path_prefix,opts.prefix) or '/'
    opts.root_dir=first(opts.root_dir,opts.root,opts.dir)
    opts.index_enabled=first(opts.index_enabled,opts.index)
    opts.path_prefix,opts.prefix,opts.root,opts.dir,opts.index=nil,nil,nil,nil,nil
  elseif name=='webdav' then
    opts.path_prefix=first(opts.path_prefix,opts.prefix) or '/'
    opts.prefix=nil
  else
    opts.file_path=first(opts.file_path,opts.file)
    opts.file=nil
  end
  opts=options(opts,nil,name=='upload_file' and native.BODY_STREAMING_UPLOAD or nil)
  return register(self,name,opts,name=='upload_file' and 'POST' or name=='webdav' and 'ANY' or {'GET','HEAD'})
end
for _,name in ipairs{'static_file','static_directory','upload_file','webdav'} do
  Server[name]=function(self,opts) return convenience(self,name,opts) end
  Group[name]=Server[name]
end
for name,method in pairs(native.methods) do
  local function verb(self,path,handler,opts)
    opts=copy(opts);opts.method=method;return route(self,path,handler,opts)
  end
  Server[name:lower()],Group[name:lower()]=verb,verb
end
function Server:group(prefix,opts)
  if type(prefix)=='table' then
    opts=prefix;prefix=opts.prefix or opts.path_prefix or ''
    if opts.routes~=nil then
      if type(prefix)~='string' or type(opts.routes)~='table' or #opts.routes==0 then return invalid('group requires a prefix and non-empty routes array') end
      local defaults=copy(opts)
      defaults.prefix,defaults.path_prefix,defaults.routes=nil,nil,nil
      for _,item in ipairs(opts.routes) do
        if type(item)~='table' or type(item.path)~='string' then return invalid('group route path required') end
        local merged=copy(defaults)
        for k,v in pairs(item) do merged[k]=v end
        local left=prefix:sub(-1)=='/' and prefix:sub(1,-2) or prefix
        local right=item.path:sub(1,1)=='/' and item.path or '/'..item.path
        merged.path=left..right
        local value,err=self:route(merged)
        if not value then return nil,err end
      end
      return true
    end
  end
  opts=options(opts);opts.prefix=prefix
  local handle,err=self._native:group(opts)
  if not handle then return nil,err end
  local normalized=prefix
  if type(normalized)=='string' and normalized:sub(-1)=='/' then normalized=normalized:sub(1,-2) end
  return setmetatable({_native=handle,_server=self,_prefix=normalized},Group)
end
function Server:run() return self._native:run() end
function Server:stop() return self._native:stop() end
function Server:close() return self._native:close() end
function Server:validate() return self._native:validate() end
function Server:effective_config() return self._native:effective_config() end
function Server:set_serving(value) return self._native:set_serving(value) end
function Server:state() return self._native:state() end
function Server:listener(index) return self._native:listener(index) end
function Server:route_count() return self._native:route_count() end
function Server:openapi_doc(opts) return self._native:openapi_doc(opts) end
function Server:openapi(opts) return self._native:openapi(opts) end
function Server:control_interest(interest) return self._native:control_interest(interest) end
function Server:tls_identity_state(index) return self._native:tls_identity_state(index) end
local function supervision_options(self,opts)
  if type(opts)~='table' then return opts end
  opts=copy(opts)
  local fn=opts.on_event
  if type(fn)=='function' then opts.on_event=function(_,events) return fn(self,events) end end
  return opts
end
function Server:supervision_take(opts) return self._native:supervision_take(supervision_options(self,opts)) end
function Server:proxy(opts)
  if type(opts)~='table' then return invalid('proxy declaration table required') end
  local handle,err=self._native:proxy(copy(opts))
  if not handle then return nil,err end
  return true
end
function Server:websocket(opts)
  if type(opts)~='table' then return invalid('websocket declaration table required') end
  opts=copy(opts)
  opts.message=first(opts.message,opts.handler);opts.handler=nil
  local handle,err=self._native:websocket(opts)
  if not handle then return nil,err end
  return true
end
for _,name in ipairs{'defer','timer','watch','waker'} do
  Server[name]=function(self,opts)
    if type(opts)=='function' then opts={callback=opts} end
    return self._native[name](self._native,opts)
  end
end
function Server:now() return self._native:now() end
local function sse(self,opts)
  if type(opts)~='table' or type(opts.read)~='function' then return invalid('SSE declaration and read callback required') end
  opts=copy(opts)
  local allowed={path=true,path_kind=true,method=true,methods=true,body=true,auth=true,before=true,after=true,
    open=true,read=true,close=true,headers=true,status=true,status_code=true,content_type=true,openapi=true}
  for key in pairs(opts) do if not allowed[key] then return invalid('unknown SSE field: '..tostring(key)) end end
  for _,name in ipairs{'open','close'} do
    if opts[name]~=nil and type(opts[name])~='function' then return invalid('SSE '..name..' must be a function') end
  end
  if opts.headers~=nil and type(opts.headers)~='table' then return invalid('SSE headers table required') end
  local headers=copy(opts.headers)
  for name,value in pairs{['cache-control']='no-cache',['x-accel-buffering']='no'} do
    local found=false
    for key,item in pairs(headers) do
      local field=type(key)=='string' and key or type(item)=='table' and item.name
      if type(field)=='string' and field:lower()==name then found=true end
    end
    if not found then
      if #headers>0 then headers[#headers+1]={name=name,value=value} else headers[name]=value end
    end
  end
  local route_opts={}
  for _,name in ipairs{'path','path_kind','method','methods','auth','before','after','openapi'} do route_opts[name]=opts[name] end
  route_opts.body='none'
  route_opts.handler=function(req)
    local state
    if opts.open then state=opts.open(req) end
    local status=opts.status_code
    if status==nil or status==0 then status=opts.status or 200 end
    return {status_code=status,content_type=opts.content_type or 'text/event-stream; charset=utf-8',headers=headers,
      stream_source={
        read=function(capacity)
          local event,err=opts.read(state,capacity)
          if err~=nil then return nil,err end
          if event==nil or event==false then return nil end
          return native.sse_encode(event,capacity)
        end,
        close=function() if opts.close then opts.close(state) end end,
      }}
  end
  return self:route(route_opts)
end
Server.sse,Group.sse=sse,sse
-- Lifted from vectis.rest's server helpers. Outbound clients stay in Vectis.
local rest={JSON_CONTENT_TYPE='application/json; charset=utf-8'}
M.rest=rest
function rest.json(value,opts)
  local response,err=native.rest.json(value,opts)
  if err then err.kind='json_encode' end
  return response,err
end
local rest_fallback='{"ok":false,"error":{"kind":"json_encode","message":"REST error response JSON encode failed",'..
  '"status":'..tostring(native.status.INVALID)..',"source_code":'..tostring(native.rest.ERROR_SOURCE_LONEJSON)..'}}\n'
local function error_value(value,opts)
  local err=type(value)=='table' and copy(value) or {message=tostring(value)}
  err.status=err.status or opts.error_status or native.status.STATE
  err.source_code=err.source_code or err.source or opts.source_code or 1
  err.message=err.message or 'request failed'
  err.kind=err.kind or 'error'
  if opts.status or opts.status_code then err.http_status=opts.status or opts.status_code end
  return err
end
local function error_http_status(err)
  if type(err.http_status)=='number' then return err.http_status end
  local status=native.status
  return err.status==status.INVALID and 400 or err.status==status.CONFLICT and 409 or
    err.status==status.NOT_SUPPORTED and 501 or err.status==status.TIMEOUT and 504 or 500
end
function rest.error_response(value,opts)
  opts=opts or {}
  local err=error_value(value,opts)
  local response,failure=rest.json({ok=false,error=err},{status=error_http_status(err),headers=opts.headers})
  if response then return response end
  response=rest.json({ok=false,error={kind='json_encode',
    message='REST error response JSON encode failed: '..tostring(failure and failure.message or failure),
    status=native.status.INVALID,source_code=native.rest.ERROR_SOURCE_LONEJSON}},
    {status=opts.fallback_status or 500,headers=opts.headers})
  return response or {status=opts.fallback_status or 500,content_type=rest.JSON_CONTENT_TYPE,
    body=rest_fallback}
end
local function route_response(value)
  if type(value)~='table' then return false end
  for _,key in ipairs{'body','file_path','file','content_type','headers','stream_source','spooled_source',
    'json','xml','json_materialized','xml_materialized'} do
    if value[key]~=nil then return true end
  end
  return type(value.status)=='number' or type(value.status_code)=='number'
end
local function rest_response(value,opts)
  if value==nil and opts==nil then return nil end
  if route_response(value) then return value end
  local response,err=rest.json(value,opts)
  return response or rest.error_response(err,{status=500})
end
local rest_keys={decode_json=true,error_handler=true,max_body_bytes=true,response=true,routes=true,validate=true,
  prefix=true,path_prefix=true}
local function rest_route_options(opts)
  if type(opts)~='table' or type(opts.handler)~='function' then
    return invalid('rotmos.rest.route requires options and handler')
  end
  opts=copy(opts)
  if opts.response~=nil then
    if type(opts.response)~='table' then return invalid('REST response options must be a table') end
    opts.response=copy(opts.response)
  end
  for _,name in ipairs{'validate','error_handler'} do
    if opts[name]~=nil and type(opts[name])~='function' then return invalid('REST '..name..' must be a function') end
  end
  if opts.decode_json~=nil and type(opts.decode_json)~='boolean' then return invalid('REST decode_json must be boolean') end
  local route={}
  for key,value in pairs(opts) do if not rest_keys[key] then route[key]=value end end
  local body=route.body
  if type(body)=='string' then body={mode=body}
  elseif body==true then body={mode='buffered'}
  elseif body==false then body={mode='none'}
  elseif type(body)=='table' then body=copy(body)
  elseif body~=nil then return invalid('REST body must be buffered or none') end
  if body==nil and opts.decode_json~=false then body={mode='buffered',max_bytes=opts.max_body_bytes or 1048576} end
  if body then
    local mode=body.mode or 'buffered'
    if mode~='buffered' and mode~=native.BODY_BUFFERED and mode~='none' and mode~=native.BODY_NONE then
      return invalid('REST body must be buffered or none')
    end
    body.mode=mode
    if mode=='buffered' or mode==native.BODY_BUFFERED then
      body.max_bytes=body.max_bytes or opts.max_body_bytes or 1048576
      body.memory_buffer_limit_bytes=body.memory_buffer_limit_bytes or 1048576
    end
    route.body=body
  end
  route.handler=function(req)
    if opts.decode_json~=false and req.body~=nil and req.body~='' then
      local decoded,value=pcall(require('lonejson').decode_json,req.body)
      if not decoded then return rest.error_response({kind='json_parse',message=tostring(value),
        status=native.status.INVALID,source_code=native.rest.ERROR_SOURCE_LONEJSON},{status=400}) end
      req.json=value
    end
    if opts.validate then
      local valid,value,detail=pcall(opts.validate,req.json,req)
      if not valid then return rest.error_response({kind='validation',message=tostring(value),
        status=native.status.INVALID},{status=400}) end
      if value~=nil and value~=true then
        if route_response(value) then return value end
        return rest.error_response(detail or value or {kind='validation',message='request validation failed',
          status=native.status.INVALID},{status=400})
      end
    end
    local handled,value,response_opts=pcall(opts.handler,req)
    if not handled then
      if opts.error_handler then
        local recovered=opts.error_handler(value,req)
        if recovered~=nil then return recovered end
      end
      return rest.error_response({kind='handler',message=tostring(value),status=native.status.STATE},{status=500})
    end
    return rest_response(value,response_opts or opts.response)
  end
  return route
end
function rest.route(server,opts)
  if type(server)~='table' or type(server.route)~='function' then return invalid('REST server or group required') end
  local route,err=rest_route_options(opts)
  if not route then return nil,err end
  return server:route(route)
end
function rest.group(server,opts)
  if type(server)~='table' or type(server.group)~='function' or type(opts)~='table' or type(opts.routes)~='table' then
    return invalid('REST server, group options and routes required')
  end
  local group={prefix=opts.prefix or opts.path_prefix,routes={}}
  for index,item in ipairs(opts.routes) do
    if type(item)~='table' then return invalid('REST group route must be a table') end
    local merged=copy(opts)
    for key,value in pairs(item) do merged[key]=value end
    local route,err=rest_route_options(merged)
    if not route then return nil,err end
    group.routes[index]=route
  end
  return server:group(group)
end
function M.server(opts)
  if opts~=nil and type(opts)~='table' then return invalid('server configuration table required') end
  opts=copy(opts)
  if opts.callbacks~=nil and type(opts.callbacks)~='table' then return invalid('callbacks table required') end
  if opts.listeners==nil and (opts.port~=nil or opts.bind~=nil) then
    opts.listeners={{bind=opts.bind or '127.0.0.1',port=opts.port or 0}}
  end
  opts.bind,opts.port=nil,nil
  if type(opts.workers)=='number' then opts.workers={count=opts.workers} end
  if opts.signals==nil then opts.signals={mode='library'} end
  local self=setmetatable({},Server)
  local callbacks=copy(opts.callbacks)
  for _,name in ipairs{'on_process_start','on_parent_prepare','on_parent_ready','on_parent_stopping','on_parent_stopped',
    'on_worker_start','on_worker_stopping','on_worker_exit','on_worker_event','on_tls_identity',
    'on_request_start','on_request_close','on_observe'} do
    local fn=opts[name]
    if fn==nil then fn=callbacks[name] end
    if fn~=nil then
      if type(fn)=='function' then
        if name=='on_request_start' then callbacks[name]=function(_,req) return fn(self,request(req,true)) end
        elseif name=='on_request_close' then callbacks[name]=function(_,req,outcome) return fn(self,request(req,true),outcome) end
        elseif name=='on_observe' then callbacks[name]=function(_,event)
          if event.request then event.request=request(event.request,true) end
          return fn(self,event)
        end
        else callbacks[name]=function(_,value) return fn(self,value) end end
      else callbacks[name]=fn end
    end
    opts[name]=nil
  end
  opts.callbacks=callbacks
  for _,name in ipairs{'client_ip','correlation_id','admission'} do
    if type(opts[name])=='table' then
      local config=copy(opts[name])
      local key=name=='admission' and 'on_request' or 'resolve'
      local fn=config[key]
      if type(fn)=='function' then config[key]=function(req) return fn(request(req,true)) end end
      opts[name]=config
    end
  end
  opts.supervision=supervision_options(self,opts.supervision)
  if type(opts.tls)=='table' and type(opts.tls.acme)=='table' then
    opts.tls=copy(opts.tls)
    opts.tls.acme=copy(opts.tls.acme)
    local fn=opts.tls.acme.state
    if type(fn)=='function' then opts.tls.acme.state=function(_,event) return fn(self,event) end end
  end
  local handle,err=native.server(opts)
  if not handle then return nil,err end
  self._native=handle
  return self
end
return M
