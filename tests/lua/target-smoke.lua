-- QEMU only proves module loading, basic public values and inert declarations.
local rotmos=require('rotmos')
local lonejson=require('lonejson')
local reply=assert(rotmos.rest.json({ok=true}))
assert(lonejson.decode_json(reply.body).ok)
local server=assert(rotmos.server{port=0,http={server_header='rotmos-smoke'}})
assert(server:get('/',function() return 'ok' end))
assert(server:route_count()==1)
assert(server:close())
