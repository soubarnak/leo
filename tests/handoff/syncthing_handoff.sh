#!/usr/bin/env bash
# Opt-in device handoff evidence (issue #19, criterion 1): two real Syncthing
# instances in rootless podman containers (A = desktop, B = phone) sync a
# synthetic NEO Library. Desktop edits go through NEO's real main.js IPC
# handlers; phone edits go through Pocket's real pocket/www/pocket-bridge.js
# (headless, over a node fs Capacitor mock). Not covered: the Android app/WebView.
# Usage: syncthing_handoff.sh [repo-root]    Exit 77 = skipped (no podman/image/node).
set -uo pipefail
ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
HERE="$ROOT/tests/handoff"
IMAGE="${LEO_SYNCTHING_IMAGE:-docker.io/syncthing/syncthing:latest}"
TIMEOUT="${LEO_SYNCTHING_TIMEOUT:-120}"
skip() { echo "SKIP: $*"; exit 77; }
fail() { echo "FAIL: $*" >&2; exit 1; }

command -v podman >/dev/null || skip "podman not installed"
command -v node >/dev/null || skip "node not installed"
command -v curl >/dev/null || skip "curl not installed"
if ! podman image exists "$IMAGE"; then
  podman pull -q "$IMAGE" >/dev/null 2>&1 || skip "cannot pull $IMAGE"
fi

RUN="$$-$RANDOM"
NET="leo-st-net-$RUN"; CA="leo-st-a-$RUN"; CB="leo-st-b-$RUN"
WORK="$(mktemp -d -t leo-syncthing-XXXXXX)"
KEY_A="leo-test-key-a-$RUN"; KEY_B="leo-test-key-b-$RUN"
cleanup() {
  podman rm -f -t 2 "$CA" "$CB" >/dev/null 2>&1
  podman network rm -f "$NET" >/dev/null 2>&1
  rm -rf "$WORK"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

mkdir -p "$WORK"/{a-lib,a-cfg,b-lib,b-cfg,out}
LIB_A="$WORK/a-lib"; LIB_B="$WORK/b-lib"
BOOK=book-1
BRIDGE="$ROOT/pocket/www/pocket-bridge.js"
neo()    { node "$ROOT/tests/neo_handoff_roundtrip.cjs" "$ROOT/main.js" "$LIB_A" "$BOOK" "$@"; }
pocket() { node "$HERE/pocket_bridge_harness.cjs" "$BRIDGE" "$LIB_B" "$BOOK" "$@"; }
tree() { (cd "$1" && find . -type f ! -path './.st*' ! -name '*.tmp' ! -name '.syncthing.*' -print0 \
  | sort -z | xargs -0 -r sha256sum); }

podman network create "$NET" >/dev/null || skip "cannot create podman network"

start() { # name key libdir cfgdir
  podman run -d --name "$1" --network "$NET" --userns=keep-id --user "$(id -u):$(id -g)" \
    --security-opt label=disable -e HOME=/var/syncthing -e STGUIAPIKEY="$2" \
    -e STGUIADDRESS=0.0.0.0:8384 -e STNOUPGRADE=1 -e STNODEFAULTFOLDER=1 \
    -p 127.0.0.1::8384 -v "$3:/data/lib" -v "$4:/var/syncthing/config" \
    "$IMAGE" >/dev/null
}
node "$HERE/make_fixture.cjs" "$LIB_A"
start "$CA" "$KEY_A" "$LIB_A" "$WORK/a-cfg" || skip "cannot start syncthing container"
start "$CB" "$KEY_B" "$LIB_B" "$WORK/b-cfg" || skip "cannot start syncthing container"
port() { podman port "$1" 8384/tcp | head -1 | sed 's/.*://'; }
PORT_A="$(port "$CA")"; PORT_B="$(port "$CB")"
api() { # port key method path [json]
  local args=(-sS -f -X "$3" -H "X-API-Key: $2" -H 'Content-Type: application/json')
  [ -n "${5:-}" ] && args+=(-d "$5")
  curl "${args[@]}" "http://127.0.0.1:$1$4"
}
wait_for() { # description, seconds, command...
  local what="$1" secs="$2"; shift 2
  local end=$((SECONDS + secs))
  until "$@" >/dev/null 2>&1; do
    [ $SECONDS -ge $end ] && return 1
    sleep 1
  done
}
for x in "$PORT_A:$KEY_A" "$PORT_B:$KEY_B"; do
  wait_for "syncthing REST" 60 api "${x%%:*}" "${x#*:}" GET /rest/system/status \
    || { podman logs "$CA" "$CB" 2>&1 | tail -20; fail "syncthing REST did not come up"; }
done
jsonget() { node -e 'let s="";process.stdin.on("data",d=>s+=d).on("end",()=>console.log(eval("("+process.argv[1]+")")(JSON.parse(s))))' "$1"; }
ID_A="$(api "$PORT_A" "$KEY_A" GET /rest/system/status | jsonget 'j=>j.myID')"
ID_B="$(api "$PORT_B" "$KEY_B" GET /rest/system/status | jsonget 'j=>j.myID')"

OPTS='{"localAnnounceEnabled":false,"globalAnnounceEnabled":false,"relaysEnabled":false,"natEnabled":false,"startBrowser":false,"urAccepted":-1,"autoUpgradeIntervalH":0}'
FOLDER() { cat <<JSON
{"id":"lib","label":"lib","path":"/data/lib","type":"sendreceive","rescanIntervalS":5,
 "fsWatcherEnabled":true,"fsWatcherDelayS":1,"devices":[{"deviceID":"$ID_A"},{"deviceID":"$ID_B"}]}
JSON
}
configure() { # port key peerId peerName peerHost
  api "$1" "$2" PATCH /rest/config/options "$OPTS" >/dev/null &&
  api "$1" "$2" PUT "/rest/config/devices/$3" \
    "{\"deviceID\":\"$3\",\"name\":\"$4\",\"addresses\":[\"tcp://$5:22000\"],\"autoAcceptFolders\":false}" >/dev/null &&
  { api "$1" "$2" DELETE /rest/config/folders/default >/dev/null 2>&1 || true; } &&
  api "$1" "$2" PUT /rest/config/folders/lib "$(FOLDER)" >/dev/null
}
configure "$PORT_A" "$KEY_A" "$ID_B" phone "$CB" || fail "configuring desktop syncthing"
configure "$PORT_B" "$KEY_B" "$ID_A" desktop "$CA" || fail "configuring phone syncthing"

synced() { [ -n "$(tree "$LIB_A")" ] && [ "$(tree "$LIB_A")" = "$(tree "$LIB_B")" ]; }
await_sync() {
  wait_for "$1" "$TIMEOUT" synced || {
    podman logs "$CA" 2>&1 | tail -15; podman logs "$CB" 2>&1 | tail -15
    diff <(tree "$LIB_A") <(tree "$LIB_B") >&2
    fail "timed out waiting for Syncthing: $1"
  }
  echo "ok: synced ($1)"
}

await_sync "initial desktop -> phone"
neo read > "$WORK/out/initial.json" || fail "NEO could not read fixture"
pocket read > "$WORK/out/pocket-read.json" || fail "Pocket bridge could not read synced Library"
node -e 'const a=require(process.argv[1]),b=require(process.argv[2]);require("assert").deepStrictEqual(b,a)' \
  "$WORK/out/initial.json" "$WORK/out/pocket-read.json" || fail "Pocket read differs from NEO read of the same files"

P1="Desktop edit one."; P2="Pocket edit on phone."; P3="Desktop edit after phone."
neo edit "$P1" > /dev/null || fail "NEO edit 1"
await_sync "desktop edit -> phone"
pocket edit "$P2" > "$WORK/out/pocket-edit.json" || fail "Pocket edit"
await_sync "phone edit -> desktop"
neo edit "$P3" > "$WORK/out/final-neo.json" || fail "NEO edit 2 (reads phone edit first)"
await_sync "desktop edit -> phone (round trip)"
pocket read > "$WORK/out/final-pocket.json" || fail "Pocket final read"

SHA_A="$(sha256sum "$LIB_A/$BOOK/unknown-supporting-data.bin" | cut -d' ' -f1)"
SHA_B="$(sha256sum "$LIB_B/$BOOK/unknown-supporting-data.bin" | cut -d' ' -f1)"
[ "$SHA_A" = "$SHA_B" ] || fail "unknown file differs between devices"
node "$HERE/assert_handoff.cjs" "$WORK/out/initial.json" "$WORK/out/final-neo.json" "$P1" "$P2" "$P3" "$SHA_A" || fail "NEO final read"
node "$HERE/assert_handoff.cjs" "$WORK/out/initial.json" "$WORK/out/final-pocket.json" "$P1" "$P2" "$P3" "$SHA_B" || fail "Pocket final read"
echo "PASS: LEO/NEO desktop <-> Pocket phone handoff over real Syncthing"
