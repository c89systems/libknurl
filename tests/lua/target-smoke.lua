-- QEMU only proves module loading, basic public values and inert declarations.
local knurl=require('knurl')
local lonejson=require('lonejson')
local reply=assert(knurl.rest.json({ok=true}))
assert(lonejson.decode_json(reply.body).ok)
local server=assert(knurl.server{port=0,http={server_header='knurl-smoke'}})
assert(server:get('/',function() return 'ok' end))
assert(server:route_count()==1)
assert(server:close())
