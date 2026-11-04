local native = require('rotmos.native')
local M = { AGAIN = native.AGAIN, EOF = native.EOF, count = native.count, status = native.status }
local Server, Group = {}, {}
Server.__index, Group.__index = Server, Group
local requests = setmetatable({}, {__mode='k'})
local function copy(t)
  local out = {}
  for k,v in pairs(t or {}) do out[k]=v end
  return out
end
local function invalid(message)
  return nil,{status=native.status.INVALID,kind='invalid argument',source=1,native_code=0,operation='lua',message=message,detail=''}
end
local function request(handle)
  local out = requests[handle]
  if out then
    out.principal=handle:principal()
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
  for _,name in ipairs{'body_reader','body_materialize','body_spill','body_copy'} do
    out[name]=function(a,b)
      if a==out then a=b end
      return handle[name](handle,a)
    end
  end
  out.body_resume=function() return handle:body_resume() end
  out.event=lookup('event')
  requests[handle]=out
  return out
end
M.source=native.source
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
local function route(self,path,handler,opts)
  if type(path)=='table' then opts=path;handler=opts.handler;path=opts.path end
  if opts~=nil and type(opts)~='table' then return invalid('route options table required') end
  opts=options(opts,handler);opts.path=path
  local handle,err=self._native:route(opts)
  if not handle then return nil,err end
  return true
end
Server.route,Group.route=route,route
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
  local handle,err=self._native:upload(opts)
  if not handle then return nil,err end
  return true
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
  local handle,err=self._native[name](self._native,opts)
  if not handle then return nil,err end
  return true
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
  return setmetatable({_native=handle,_server=self},Group)
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
function Server:proxy(opts)
  if type(opts)~='table' then return invalid('proxy declaration table required') end
  local handle,err=self._native:proxy(copy(opts))
  if not handle then return nil,err end
  return true
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
  for _,name in ipairs{'on_process_start','on_parent_prepare','on_parent_ready','on_parent_stopping','on_parent_stopped','on_worker_start','on_worker_stopping','on_worker_exit'} do
    local fn=opts[name]
    if fn==nil then fn=callbacks[name] end
    if fn~=nil then
      if type(fn)=='function' then callbacks[name]=function(_,process) return fn(self,process) end
      else callbacks[name]=fn end
    end
    opts[name]=nil
  end
  opts.callbacks=callbacks
  local handle,err=native.server(opts)
  if not handle then return nil,err end
  self._native=handle
  return self
end
return M
