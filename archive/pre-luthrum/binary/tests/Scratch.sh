#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)
binary=${1:?binary required}
work=${2:?owned test directory required}
fixtures=${3:-$(dirname -- "$(dirname -- "$binary")")/tests/mmdb-fixtures}
orphan=${4:-$(dirname -- "$binary")/knurl_test_orphan}
cmake "-DKNURL_CHECK_PATH=$work" -P "$root/cmake/OwnedPaths.cmake"
mkdir -p "$work/context/www"
context="$work/context"
container= image_positive= image_negative=
cleanup() {
  if [[ -n "$container" ]]; then podman logs "$container" >"$work/container.log" 2>&1 || true; podman rm -f "$container" >/dev/null; fi
  if [[ -n "$image_positive" ]]; then podman rmi "$image_positive" >/dev/null; fi
  if [[ -n "$image_negative" ]]; then podman rmi "$image_negative" >/dev/null; fi
}
trap cleanup EXIT
cp "$binary" "$context/knurl"
cp "$orphan" "$context/orphan"
cp "$root/binary/tests/server.lua" "$context/server.lua"
cp "$fixtures/valid-24-4.mmdb" "$context/country.mmdb"
printf '0123456789' >"$context/www/data.txt"
openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj /CN=Knurl-fixture-CA \
  -keyout "$work/ca.key" -out "$context/ca.pem" >"$work/openssl.log" 2>&1
openssl req -newkey rsa:2048 -nodes -subj /CN=localhost -keyout "$context/key.pem" \
  -out "$work/server.csr" >>"$work/openssl.log" 2>&1
printf 'subjectAltName=DNS:localhost,IP:127.0.0.1\n' >"$work/extensions"
openssl x509 -req -in "$work/server.csr" -CA "$context/ca.pem" -CAkey "$work/ca.key" \
  -CAcreateserial -days 2 -extfile "$work/extensions" -out "$context/cert.pem" >>"$work/openssl.log" 2>&1
podman build --iidfile "$work/positive.iid" --build-arg WITH_TEST_CA=1 \
  -f "$root/binary/tests/Containerfile" "$context" >"$work/build-positive.log" 2>&1
image_positive=$(cat "$work/positive.iid")
podman build --iidfile "$work/negative.iid" --build-arg WITH_TEST_CA=0 \
  -f "$root/binary/tests/Containerfile" "$context" >"$work/build-negative.log" 2>&1
image_negative=$(cat "$work/negative.iid")
start() {
  local image=$1
  shift
  container=$(podman run -d --read-only --tmpfs /tmp:rw,mode=1777 --cap-drop all \
    -p 127.0.0.1::8080 "$@" "$image")
  port=$(podman port "$container" 8080/tcp)
  url="http://$port"
  for ((attempt=0;attempt<100;attempt++)); do
    if curl --noproxy '*' -fsS --max-time 1 "$url/ready" >"$work/ready" 2>/dev/null; then return; fi
    sleep 0.05
  done
  podman logs "$container" >&2
  printf 'scratch server failed readiness\n' >&2; exit 1
}
exchange() {
  local status
  status=$(curl --noproxy '*' -sS --max-time 5 -o "$work/body" -w '%{http_code}' "$url$1")
  [[ "$status" == "$2" ]] || { cat "$work/body" >&2; printf 'Expected %s at %s; got %s\n' "$2" "$1" "$status" >&2; exit 1; }
  [[ -z "${3:-}" ]] || rg -Fq -- "$3" "$work/body"
}
start "$image_positive" --entrypoint /test/orphan
exchange /pid1 200 knurl
exchange /api 200 verified
exchange /wrong-ca 200 rejected
exchange /proxy 200 verified
exchange /deny 403
exchange /country 200 country
for ((attempt=0;attempt<100;attempt++)); do
  if [[ $(curl --noproxy '*' -fsS "$url/orphan") == reaped ]]; then break; fi
  sleep 0.05
done
[[ "$attempt" -lt 100 ]]
[[ $(curl --noproxy '*' -fsS -H 'Range: bytes=2-5' "$url/static/data.txt") == 2345 ]]
[[ $(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' "$url/dav/file") == 401 ]]
[[ $(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' -X PUT -H 'Authorization: Bearer fixture-key' --data-binary 'dav-data' "$url/dav/file") == 201 ]]
[[ $(curl --noproxy '*' -fsS -H 'Authorization: Bearer fixture-key' "$url/dav/file") == dav-data ]]
podman kill --signal TERM "$container" >/dev/null
[[ $(timeout 12 podman wait "$container") == 0 ]]
podman logs "$container" >"$work/positive.log" 2>&1
rg -q scratch-parent-stopped "$work/positive.log"
podman rm "$container" >/dev/null; container=
start "$image_negative"
exchange /api 502
exchange /wrong-ca 200 rejected
podman kill --signal TERM "$container" >/dev/null
[[ $(timeout 12 podman wait "$container") == 0 ]]
podman rm "$container" >/dev/null; container=
start "$image_positive" -e INJECT_STOP_HANG=1 -e PID0_STOP_TIMEOUT=1s
podman kill --signal TERM "$container" >/dev/null
[[ $(timeout 12 podman wait "$container") == 137 ]]
podman rm "$container" >/dev/null; container=
[[ $(podman run --rm "$image_positive" --version) == knurl\ * ]]
set +e
podman run --rm "$image_positive" /absent.lua >"$work/exit.log" 2>&1
status=$?
set -e
[[ "$status" == 1 ]]
printf 'Scratch static runtime, modules, system trust, route curl, proxy, ACL, ranges, WebDAV and PID 1 TERM/exit passed\n'
