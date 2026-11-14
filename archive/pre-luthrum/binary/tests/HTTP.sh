#!/usr/bin/env bash
set -euo pipefail
binary=${1:?binary required}
work=${2:?test directory required}
emulator=${3:-}
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)
cmake "-DKNURL_CHECK_PATH=$work" -P "$root/cmake/OwnedPaths.cmake"
mkdir -p "$work"
rm -f "$work/port"
cat >"$work/server.lua" <<'LUA'
local k=require('knurl')
local curl=require('async.curl')
local function must(v,e) assert(v,e and e.message);return v end
local app,port
app=must(k.server{port=0,http={backend_quiet=true},on_parent_ready=function(s)
  port=must(s:listener()).port
  local f=assert(io.open(os.getenv('KNURL_TEST_PORT'),'w'));assert(f:write(tostring(port)));assert(f:close())
end})
must(app:get('/ok',function() return 'ok' end))
must(app:get('/fetch',function()
  local p=must(app:listener()).port
  local r=curl.perform{url='http://127.0.0.1:'..p..'/ok',timeout_ms=2000}
  assert(r.ok and r.body=='ok');return 'fetched'
end))
must(app:get('/failure',function() error('intentional-route-error') end))
must(app:get('/stream',function()
  local remaining=32
  return {stream_source=must(k.source{read=function(capacity)
    if remaining==0 then return nil,k.EOF end
    remaining=remaining-1;return string.rep('x',math.min(capacity,1024))
  end})}
end))
must(app:get('/stop',function() must(app:stop());return 'stopped' end))
return app
LUA
runner=()
[[ -z "$emulator" ]] || runner=("$emulator")
KNURL_TEST_PORT="$work/port" LUA_PATH=/no/lua LUA_CPATH=/no/native \
  "${runner[@]}" "$binary" "$work/server.lua" >"$work/server.log" 2>&1 &
server_pid=$!
cleanup() { kill "$server_pid" 2>/dev/null || true; wait "$server_pid" 2>/dev/null || true; }
trap cleanup EXIT
for ((attempt=0;attempt<100;attempt++)); do
  [[ -s "$work/port" ]] && break
  if ! kill -0 "$server_pid" 2>/dev/null; then cat "$work/server.log" >&2; exit 1; fi
  sleep 0.05
done
[[ -s "$work/port" ]]
url="http://127.0.0.1:$(cat "$work/port")"
[[ $(curl --noproxy '*' -fsS --max-time 5 "$url/ok") == ok ]]
[[ $(curl --noproxy '*' -fsS --max-time 5 "$url/fetch") == fetched ]]
[[ $(curl --noproxy '*' -sS --max-time 5 -o "$work/error" -w '%{http_code}' "$url/failure") == 500 ]]
[[ ! -s "$work/error" ]]
curl --noproxy '*' -fsS --max-time 5 "$url/stream" -o "$work/stream"
[[ $(wc -c <"$work/stream") == 32768 ]]
[[ $(curl --noproxy '*' -fsS --max-time 5 "$url/stop") == stopped ]]
wait "$server_pid"
trap - EXIT
