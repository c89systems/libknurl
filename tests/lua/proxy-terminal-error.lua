proxy_terminal_mode='error'
local path=debug.getinfo(1,'S').source:sub(2):match('^(.*[/])')
dofile(path..'proxy.lua')
