#!/usr/bin/env bash
# Opt-in device handoff evidence on a real Android stack (issue #19, criterion 1):
# the real NEO Pocket debug APK and the official Syncthing Android app run in an
# Android emulator (API 35, google_apis x86_64, KVM-accelerated) inside a rootless
# podman container; a desktop Syncthing (official image) runs in a second container
# sharing the emulator's network namespace. A synthetic NEO Library is seeded on
# the desktop side and synced into /storage/emulated/0/Documents/NEO Library by the
# real Syncthing Android app. The desktop edits go through NEO's real main.js
# handlers (tests/neo_handoff_roundtrip.cjs); the phone edit is typed into Pocket's
# real WebView with adb touch/key events (Chrome DevTools is used only to find
# element positions and read state). Evidence goes to a gitignored directory.
#
# This is an EMULATOR run, not a physical phone: it does not cover vendor Android
# builds, a real Wi-Fi network, relays/discovery, or battery/doze behavior.
#
# Usage: android_pocket_handoff.sh [repo-root]        Exit 77 = skipped.
# Environment:
#   LEO_ANDROID_CACHE             big caches (SDK, image, APKs); default ~/.cache/leo-android-cache
#   LEO_ANDROID_EVIDENCE          evidence directory; default tests/handoff/android-evidence/<timestamp>
#   LEO_ANDROID_ACCEPT_LICENSES=1 accept the Android SDK licenses (needed only for the first SDK download)
#   LEO_SYNCTHING_IMAGE           desktop Syncthing image
#   LEO_ANDROID_TIMEOUT           seconds to wait for each Syncthing sync (default 180)
set -uo pipefail
ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
HERE="$ROOT/tests/handoff"
AND="$HERE/android"
CACHE="${LEO_ANDROID_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/leo-android-cache}"
IMAGE="${LEO_SYNCTHING_IMAGE:-docker.io/syncthing/syncthing:latest}"
TIMEOUT="${LEO_ANDROID_TIMEOUT:-180}"
STAMP="$(date +%Y%m%d-%H%M%S)"
EVID="${LEO_ANDROID_EVIDENCE:-$HERE/android-evidence/$STAMP}"
TOOLBOX=localhost/leo-android-toolbox

# Pinned downloads (official sources; sha256/sha1 as published by the vendors).
CMDLINE_ZIP=commandlinetools-linux-13114758_latest.zip
CMDLINE_SHA1=5fdcc763663eefb86a5b8879697aa6088b041e70
SYS_IMAGE='system-images;android-35;google_apis;x86_64'
CRUN_VER=1.30.1
CRUN_SHA256=86d1e6a0e76945975d3aebfab39cbc6a26eea15f1c3fc66b6776d19e5dc346a0
ST_APK_VER=1.28.1
ST_APK_SHA256=742984454612f382fb6ba7f7f6fc5f309161cc05a1f4692945d70644cf0a9324

skip() { echo "SKIP: $*"; exit 77; }
fail() { echo "FAIL: $*" >&2; exit 1; }
step() { echo "== $*"; }

command -v podman >/dev/null || skip "podman not installed"
command -v node >/dev/null || skip "node not installed"
command -v curl >/dev/null || skip "curl not installed"
command -v rsync >/dev/null || skip "rsync not installed"
# slirp4netns: container downloads use it because rootless pasta networking can lack outbound connectivity
command -v slirp4netns >/dev/null || skip "slirp4netns not installed"
[ -r /dev/kvm ] && [ -w /dev/kvm ] || skip "/dev/kvm is not readable and writable by this user"

mkdir -p "$CACHE"/{bin,dl,sdk,gradle-home,home,avd,apk} "$EVID" || skip "cannot create cache/evidence dirs"
exec > >(tee -a "$EVID/run.log") 2>&1
echo "evidence: $EVID   cache: $CACHE"

# --- container runtime: /dev/kvm needs `--group-add keep-groups`, which only crun supports
PM=(podman)
if ! command -v crun >/dev/null; then
  CRUN="$CACHE/bin/crun"
  if [ ! -x "$CRUN" ]; then
    step "downloading crun $CRUN_VER (static, containers/crun release)"
    curl -fsSL -o "$CRUN.part" "https://github.com/containers/crun/releases/download/$CRUN_VER/crun-$CRUN_VER-linux-amd64" \
      || skip "cannot download crun"
    [ "$(sha256sum "$CRUN.part" | cut -d' ' -f1)" = "$CRUN_SHA256" ] || { rm -f "$CRUN.part"; fail "crun checksum mismatch"; }
    chmod +x "$CRUN.part"; mv "$CRUN.part" "$CRUN"
  fi
  PM=(podman --runtime "$CRUN")
fi
pm() { "${PM[@]}" "$@"; }
UIDGID="$(id -u):$(id -g)"
tbrun() { pm run --rm -i --network slirp4netns --userns=keep-id --user "$UIDGID" --security-opt label=disable \
  -e HOME=/work/home -e GRADLE_USER_HOME=/work/gradle-home -e ANDROID_AVD_HOME=/work/avd \
  -v "$CACHE/sdk:/opt/android-sdk" -v "$CACHE:/work" "$@"; }

# --- one-time caches (idempotent)
if ! pm image exists "$TOOLBOX"; then
  step "building $TOOLBOX"
  pm build --network slirp4netns -q -t "$TOOLBOX" -f "$AND/Containerfile" "$AND" >/dev/null || skip "cannot build toolbox image"
fi
if ! pm image exists "$IMAGE"; then
  pm pull -q "$IMAGE" >/dev/null 2>&1 || skip "cannot pull $IMAGE"
fi
SDK="$CACHE/sdk"
if [ ! -x "$SDK/emulator/emulator" ] || [ ! -d "$SDK/platforms/android-36" ] || [ ! -d "$SDK/system-images/android-35/google_apis/x86_64" ]; then
  [ "${LEO_ANDROID_ACCEPT_LICENSES:-}" = 1 ] || skip "Android SDK (~5 GB) missing; set LEO_ANDROID_ACCEPT_LICENSES=1 to download it and accept the Android SDK licenses"
  step "installing Android SDK into $SDK"
  if [ ! -x "$SDK/cmdline-tools/latest/bin/sdkmanager" ]; then
    curl -fsSL -o "$CACHE/dl/$CMDLINE_ZIP" "https://dl.google.com/android/repository/$CMDLINE_ZIP" || skip "cannot download cmdline-tools"
    [ "$(sha1sum "$CACHE/dl/$CMDLINE_ZIP" | cut -d' ' -f1)" = "$CMDLINE_SHA1" ] || fail "cmdline-tools checksum mismatch"
    rm -rf "$SDK/cmdline-tools"; mkdir -p "$SDK/cmdline-tools"
    tbrun "$TOOLBOX" sh -c "unzip -q /work/dl/$CMDLINE_ZIP -d /opt/android-sdk/cmdline-tools && mv /opt/android-sdk/cmdline-tools/cmdline-tools /opt/android-sdk/cmdline-tools/latest" </dev/null || fail "unpacking cmdline-tools"
  fi
  yes | tbrun "$TOOLBOX" sdkmanager --licenses >/dev/null 2>&1
  tbrun "$TOOLBOX" sdkmanager platform-tools emulator "platforms;android-36" "build-tools;36.0.0" "$SYS_IMAGE" </dev/null 2>&1 | tail -c 400 \
    || fail "sdkmanager"
fi
if [ ! -d "$CACHE/avd/leo.avd" ]; then
  step "creating AVD"
  echo no | tbrun "$TOOLBOX" avdmanager create avd -n leo -k "$SYS_IMAGE" -d pixel_6 --force >/dev/null 2>&1 || fail "avdmanager"
fi

POCKET_APK="$CACHE/apk/neo-pocket-debug.apk"
src_newer() { [ ! -f "$POCKET_APK" ] || [ -n "$(find "$ROOT/pocket/www" "$ROOT/pocket/android/app/src" "$ROOT/pocket/package.json" \
  "$ROOT/pocket/capacitor.config.json" "$ROOT/app.js" "$ROOT/covers.js" "$ROOT/styles.css" -type f -newer "$POCKET_APK" \
  ! -path '*/www/app.js' ! -path '*/www/covers.js' ! -path '*/www/styles.css' -print -quit)" ]; }
if src_newer; then
  step "building Pocket debug APK in podman"
  mkdir -p "$CACHE/pocket-src"
  rsync -a --delete --exclude node_modules --exclude android/build --exclude android/app/build --exclude android/.gradle \
    "$ROOT/pocket/" "$CACHE/pocket-src/" || fail "rsync pocket"
  cp "$ROOT/app.js" "$ROOT/covers.js" "$ROOT/styles.css" "$CACHE/pocket-src/www/"
  tbrun -w /work/pocket-src "$TOOLBOX" sh -c 'npm ci --no-audit --no-fund >/dev/null 2>&1 && npx cap sync android >/dev/null 2>&1 \
    && cd android && ./gradlew --no-daemon -q assembleDebug' </dev/null 2>&1 | tail -n 20
  cp "$CACHE/pocket-src/android/app/build/outputs/apk/debug/app-debug.apk" "$POCKET_APK" || fail "Pocket APK build failed"
fi
ST_APK="$CACHE/dl/syncthing-android-$ST_APK_VER.apk"
if [ ! -f "$ST_APK" ]; then
  step "downloading Syncthing Android $ST_APK_VER (official syncthing/syncthing-android release)"
  curl -fsSL -o "$ST_APK.part" "https://github.com/syncthing/syncthing-android/releases/download/$ST_APK_VER/app-release.apk" \
    || skip "cannot download Syncthing Android APK"
  [ "$(sha256sum "$ST_APK.part" | cut -d' ' -f1)" = "$ST_APK_SHA256" ] || { rm -f "$ST_APK.part"; fail "Syncthing APK checksum mismatch"; }
  mv "$ST_APK.part" "$ST_APK"
fi

# --- per-run state
RUN="$$-$RANDOM"
EMU="leo-android-emu-$RUN"; DESK="leo-android-desk-$RUN"
RUNDIR="$CACHE/run-$RUN"
LIB_A="$RUNDIR/desktop-lib"; CFG_A="$RUNDIR/desktop-cfg"
mkdir -p "$LIB_A" "$CFG_A" "$RUNDIR/out"
KEY_A="leo-test-key-$RUN"
BOOK=book-1
PKG=com.hughhowey.neopocket; STPKG=com.nutomic.syncthingandroid
PHONE_LIB='/storage/emulated/0/Documents/NEO Library'
START=$SECONDS

adb() { pm exec "$EMU" adb "$@"; }
adbsh() { pm exec "$EMU" adb shell "$@" | tr -d '\r'; }
cleanup() {
  local rc=$?
  if pm container exists "$EMU" 2>/dev/null; then
    adb logcat -d > "$EVID/logcat.txt" 2>&1
    pm logs "$EMU" > "$EVID/emulator-container.log" 2>&1
    adb emu kill >/dev/null 2>&1; sleep 2
  fi
  pm container exists "$DESK" 2>/dev/null && pm logs "$DESK" > "$EVID/desktop-syncthing.log" 2>&1
  pm rm -f -t 2 "$DESK" >/dev/null 2>&1
  pm rm -f -t 2 "$EMU" >/dev/null 2>&1
  [ -d "$RUNDIR/out" ] && cp -r "$RUNDIR/out" "$EVID/snapshots" 2>/dev/null
  rm -rf "$RUNDIR"
  echo "elapsed: $((SECONDS - START))s  exit: $rc"
  exit $rc
}
trap cleanup EXIT
trap 'exit 130' INT TERM

neo() { node "$ROOT/tests/neo_handoff_roundtrip.cjs" "$ROOT/main.js" "$LIB_A" "$BOOK" "$@"; }
wait_for() { local secs="$1"; shift; local end=$((SECONDS + secs)); until "$@" >/dev/null 2>&1; do [ $SECONDS -ge $end ] && return 1; sleep 2; done; }
shot() { adb exec-out screencap -p > "$EVID/$1.png" 2>/dev/null; }
udump() { adb shell uiautomator dump /sdcard/u.xml >/dev/null 2>&1; adb shell cat /sdcard/u.xml; }
uitap() { local xy; xy="$(udump | node "$AND/ui.cjs" "$1")" || return 1; adb shell input tap $xy; }
jsonget() { node -e 'let s="";process.stdin.on("data",d=>s+=d).on("end",()=>console.log(eval("("+process.argv[1]+")")(JSON.parse(s))))' "$1"; }

step "desktop: seed synthetic Library and start desktop Syncthing"
node "$HERE/make_fixture.cjs" "$LIB_A"
neo read > "$RUNDIR/out/initial.json" || fail "NEO could not read fixture"

step "phase 2: boot emulator (API 35 google_apis x86_64, KVM)"
pm run -d --name "$EMU" --network slirp4netns -p 127.0.0.1::8384 --device /dev/kvm --group-add keep-groups \
  --userns=keep-id --user "$UIDGID" --security-opt label=disable -e HOME=/work/home -e ANDROID_AVD_HOME=/work/avd \
  -v "$SDK:/opt/android-sdk" -v "$CACHE:/work" "$TOOLBOX" \
  emulator -avd leo -no-window -no-audio -no-boot-anim -no-snapshot -wipe-data -gpu swiftshader_indirect \
  -memory 3072 -cores 4 -no-metrics -accel on -port 5554 >/dev/null || fail "cannot start emulator container"
pm run -d --name "$DESK" --network "container:$EMU" --userns=keep-id --user "$UIDGID" --security-opt label=disable \
  -e HOME=/var/syncthing -e STGUIAPIKEY="$KEY_A" -e STGUIADDRESS=0.0.0.0:8384 -e STNOUPGRADE=1 -e STNODEFAULTFOLDER=1 \
  -v "$LIB_A:/data/lib" -v "$CFG_A:/var/syncthing/config" "$IMAGE" >/dev/null || fail "cannot start desktop syncthing"
booted() { [ "$(adbsh getprop sys.boot_completed)" = 1 ]; }
wait_for 420 booted || { pm logs "$EMU" 2>&1 | tail -20; fail "emulator did not boot (sys.boot_completed)"; }
adbsh settings put secure immersive_mode_confirmations confirmed >/dev/null
adbsh settings put global hide_error_dialogs 1 >/dev/null   # emulator under load can trigger ANR dialogs
adbsh settings put global window_animation_scale 0 >/dev/null
adbsh settings put global transition_animation_scale 0 >/dev/null
adbsh settings put global animator_duration_scale 0 >/dev/null
API="$(adbsh getprop ro.build.version.sdk)"; REL="$(adbsh getprop ro.build.version.release)"
WEBVIEW="$(adbsh dumpsys webviewupdate | sed -n 's/.*Current WebView package (name, version): (\(.*\)).*/\1/p')"
echo "ok: emulator booted, Android $REL (API $API), WebView $WEBVIEW"

step "phase 3: install Pocket + Syncthing Android, grant access, pair, share folder"
adb install -g -r /work/apk/neo-pocket-debug.apk 2>&1 | tail -1 | grep -q Success || fail "installing Pocket APK"
adb install -g -r "/work/dl/$(basename "$ST_APK")" 2>&1 | tail -1 | grep -q Success || fail "installing Syncthing APK"
for p in $PKG $STPKG; do adbsh appops set $p MANAGE_EXTERNAL_STORAGE allow; done
[ "$(adbsh appops get $PKG MANAGE_EXTERNAL_STORAGE)" = "MANAGE_EXTERNAL_STORAGE: allow" ] || fail "All files access not granted to Pocket"
ST_VER="$(adbsh dumpsys package $STPKG | sed -n 's/.*versionName=//p' | head -1)"
POCKET_VER="$(adbsh dumpsys package $PKG | sed -n 's/.*versionName=//p' | head -1)"

adbsh am start -n $STPKG/.activities.FirstStartActivity >/dev/null
at_main() { udump | node "$AND/ui.cjs" id=add_folder; }
for i in $(seq 1 40); do
  at_main >/dev/null 2>&1 && break
  d="$(udump)"
  if xy="$(echo "$d" | node "$AND/ui.cjs" id=btn_next)"; then adb shell input tap $xy
  elif xy="$(echo "$d" | node "$AND/ui.cjs" "text=Don't show again")"; then adb shell input tap $xy
  elif xy="$(echo "$d" | node "$AND/ui.cjs" "text=^Wait$")"; then adb shell input tap $xy
  fi
  sleep 2
done
at_main >/dev/null 2>&1 || { shot syncthing-wizard-stuck; fail "Syncthing Android first-run wizard did not reach the main screen"; }
shot 01-syncthing-android-main
adb root >/dev/null 2>&1; adb wait-for-device
KEY_B="$(adbsh "grep -o '<apikey>[^<]*' /data/data/$STPKG/files/config.xml" | tr -d '\n' | sed 's/.*>//')"
[ -n "$KEY_B" ] || fail "cannot read Syncthing Android API key"
adb forward tcp:8385 tcp:8384 >/dev/null
PORT_A="$(pm port "$EMU" 8384/tcp | head -1 | sed 's/.*://')"
apiA() { curl -sS -f -X "$1" -H "X-API-Key: $KEY_A" -H 'Content-Type: application/json' ${3:+-d "$3"} "http://127.0.0.1:$PORT_A$2"; }
apiB() { pm exec "$EMU" curl -sSk -f -X "$1" -H "X-API-Key: $KEY_B" -H 'Content-Type: application/json' ${3:+-d "$3"} "https://127.0.0.1:8385$2"; }
wait_for 90 apiA GET /rest/system/status || fail "desktop syncthing REST did not come up"
wait_for 90 apiB GET /rest/system/status || fail "Syncthing Android REST did not come up"
ID_A="$(apiA GET /rest/system/status | jsonget 'j=>j.myID')"; ID_B="$(apiB GET /rest/system/status | jsonget 'j=>j.myID')"
OPTS='{"localAnnounceEnabled":false,"globalAnnounceEnabled":false,"relaysEnabled":false,"natEnabled":false,"startBrowser":false,"urAccepted":-1,"autoUpgradeIntervalH":0}'
FOLDER() { echo "{\"id\":\"lib\",\"label\":\"lib\",\"path\":\"$1\",\"type\":\"sendreceive\",\"rescanIntervalS\":5,\"fsWatcherEnabled\":true,\"fsWatcherDelayS\":1,\"devices\":[{\"deviceID\":\"$ID_A\"},{\"deviceID\":\"$ID_B\"}]}"; }
apiA PATCH /rest/config/options "$OPTS" >/dev/null && apiB PATCH /rest/config/options "$OPTS" >/dev/null || fail "syncthing options"
# the phone dials the desktop at the emulator's host alias (10.0.2.2 = the shared network namespace)
apiA PUT "/rest/config/devices/$ID_B" "{\"deviceID\":\"$ID_B\",\"name\":\"phone\",\"addresses\":[\"dynamic\"]}" >/dev/null || fail "desktop: add phone"
apiB PUT "/rest/config/devices/$ID_A" "{\"deviceID\":\"$ID_A\",\"name\":\"desktop\",\"addresses\":[\"tcp://10.0.2.2:22000\"]}" >/dev/null || fail "phone: add desktop"
for f in $(apiB GET /rest/config/folders | jsonget 'j=>j.map(f=>f.id).join(" ")'); do apiB DELETE "/rest/config/folders/$f" >/dev/null; done
apiA DELETE /rest/config/folders/default >/dev/null 2>&1 || true
apiA PUT /rest/config/folders/lib "$(FOLDER /data/lib)" >/dev/null || fail "desktop: add folder"
apiB PUT /rest/config/folders/lib "$(FOLDER "$PHONE_LIB")" >/dev/null || fail "phone: add folder"

# desktop tree vs phone tree (sha256 of every Library file, Syncthing bookkeeping excluded)
tree_a() { (cd "$LIB_A" && find . -type f ! -path './.st*' ! -name '*.tmp' ! -name '.syncthing.*' -print0 | xargs -0 -r sha256sum) | LC_ALL=C sort -k2; }
tree_b() { adbsh "cd '$PHONE_LIB' && find . -type f ! -path './.st*' ! -name '*.tmp' ! -name '.syncthing.*' -exec sha256sum {} +" | LC_ALL=C sort -k2; }
synced() { [ -n "$(tree_a)" ] && [ "$(tree_a)" = "$(tree_b)" ] && [ "$(apiB GET '/rest/db/status?folder=lib' | jsonget 'j=>j.state+j.needTotalItems')" = idle0 ]; }
await_sync() {
  wait_for "$TIMEOUT" synced || {
    diff <(tree_a) <(tree_b) >&2; apiB GET /rest/system/connections | head -c 600 >&2
    shot "sync-failed-$1"; fail "timed out waiting for Syncthing: $1"
  }
  echo "ok: synced ($1)"
}
await_sync "initial desktop -> phone"
shot 02-syncthing-android-up-to-date
echo "ok: real Syncthing Android delivered the Library into $PHONE_LIB"

step "phase 4: drive the real handoff"
# --- Pocket helpers: the real app in the real WebView
launch_pocket() {
  adbsh am force-stop $PKG; adbsh am start -n $PKG/.MainActivity >/dev/null
  local pid=""; for _ in $(seq 1 30); do pid="$(adbsh pidof $PKG)"; [ -n "$pid" ] && break; sleep 1; done
  [ -n "$pid" ] || fail "Pocket did not start"
  for _ in $(seq 1 30); do adb shell cat /proc/net/unix | grep -q "webview_devtools_remote_$pid" && break; sleep 1; done
  adb forward tcp:9222 localabstract:webview_devtools_remote_$pid >/dev/null
  wait_for 60 cdp 'document.querySelectorAll(".book").length > 0 && typeof window.neo === "object"' || { shot pocket-no-shelf; fail "Pocket shelf did not render"; }
  read -r WV_LEFT WV_TOP < <(udump | node "$AND/ui.cjs" webview); WV_LEFT=${WV_LEFT:-0}; WV_TOP=${WV_TOP:-0}
}
cdp() { pm exec -i "$EMU" node - 9222 "$1" < "$AND/cdp.cjs"; }
tapsel() { # real touch at the centre of a DOM element
  local xy; xy="$(cdp "(()=>{const e=document.querySelector('$1');e.scrollIntoView({block:'center'});const r=e.getBoundingClientRect();const d=devicePixelRatio;return Math.round($WV_LEFT+r.x*d+r.width*d/2)+' '+Math.round($WV_TOP+r.y*d+r.height*d/2)})()" | tr -d '"')" || return 1
  adb shell input tap $xy
}
pocket_snapshot() { cdp "(async()=>{const n=window.neo;const library=await n.readLibrary();const book=await n.readBookMeta('$BOOK');const chapters={};for(const id of book.chapterOrder)chapters[id]=await n.readChapter('$BOOK',id);return {library,book,chapters,stickies:await n.readJSON('$BOOK','stickies',[]),darlings:await n.readJSON('$BOOK','darlings',[])}})()"; }

launch_pocket
shot 03-pocket-shelf
pocket_snapshot > "$RUNDIR/out/pocket-initial.json" || fail "Pocket could not read the synced Library"
node -e 'const a=require(process.argv[1]),b=require(process.argv[2]);require("assert").deepStrictEqual(b,a)' \
  "$RUNDIR/out/initial.json" "$RUNDIR/out/pocket-initial.json" || fail "Pocket (on device) reads different data than NEO from the same files"
echo "ok: Pocket on device reads the synced Library identically to NEO"
adbsh am force-stop $PKG

P1="Desktop edit one."; P2="Pocket edit on phone."; P3="Desktop edit after phone."
neo edit "$P1" >/dev/null || fail "NEO edit 1"
await_sync "desktop edit -> phone"

launch_pocket
tapsel .book || fail "cannot tap the book on the shelf"
wait_for 30 cdp 'document.querySelectorAll(".chapter-body").length === 2' || { shot pocket-open-failed; cdp 'document.getElementById("hint").textContent' >&2; fail "Pocket did not open the book"; }
sleep 1; shot 04-pocket-book-open
cdp 'document.querySelector(".chapter[data-id=chapter-a] .chapter-body").textContent.includes("Desktop edit one.")' | grep -q true \
  || fail "Pocket did not show the desktop edit after sync"
tapsel '.chapter[data-id="chapter-a"] .chapter-body p:last-child' || fail "cannot tap into the chapter"
sleep 1
adb shell input keycombination 113 123   # Ctrl+End: caret to the end of the chapter
sleep 1; adb shell input keyevent 66     # Enter: new paragraph
adb shell input text 'Pocket%sedit%son%sphone.'
sleep 3                                  # > 2 s autosave
shot 05-pocket-after-typing
adbsh cat "'$PHONE_LIB/$BOOK/chapters/chapter-a.html'" | grep -q "<p>$P2</p>" || fail "Pocket did not autosave the typed edit"
echo "ok: typed edit autosaved by Pocket"
tapsel '#back-to-shelf' || fail "cannot tap Shelf"
wait_for 10 cdp '!document.getElementById("bookshelf-view").hidden' || fail "Pocket did not return to the shelf"
sleep 2; shot 06-pocket-back-on-shelf
adb shell input keyevent KEYCODE_HOME; sleep 1; adbsh am force-stop $PKG   # close Pocket
await_sync "phone edit -> desktop"

neo edit "$P3" > "$RUNDIR/out/final-neo.json" || fail "NEO edit 2 (reads phone edit first)"
await_sync "desktop edit -> phone (round trip)"
launch_pocket
pocket_snapshot > "$RUNDIR/out/final-pocket.json" || fail "Pocket final read"
tapsel .book; wait_for 30 cdp 'document.querySelectorAll(".chapter-body").length === 2'; sleep 1; shot 07-pocket-final-book

SHA_A="$(sha256sum "$LIB_A/$BOOK/unknown-supporting-data.bin" | cut -d' ' -f1)"
SHA_B="$(adbsh sha256sum "'$PHONE_LIB/$BOOK/unknown-supporting-data.bin'" | cut -d' ' -f1)"
[ "$SHA_A" = "$SHA_B" ] || fail "unknown file differs between devices"
export HANDOFF_REAL_EDITOR=1
node "$HERE/assert_handoff.cjs" "$RUNDIR/out/initial.json" "$RUNDIR/out/final-neo.json" "$P1" "$P2" "$P3" "$SHA_A" || fail "NEO final read"
node "$HERE/assert_handoff.cjs" "$RUNDIR/out/initial.json" "$RUNDIR/out/final-pocket.json" "$P1" "$P2" "$P3" "$SHA_B" || fail "Pocket final read (on device)"
adb pull "$PHONE_LIB/$BOOK/chapters/chapter-a.html" "/work/run-$RUN/out/phone-chapter-a.html" >/dev/null 2>&1

cat > "$EVID/summary.txt" <<EOF
Android Pocket handoff: PASS
date:            $(date -u +%Y-%m-%dT%H:%M:%SZ)
leo commit:      $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null) $(git -C "$ROOT" diff --quiet 2>/dev/null || echo '(+uncommitted changes)')
device:          Android emulator (NOT a physical phone), Android $REL (API $API), google_apis x86_64, KVM
WebView:         $WEBVIEW
Pocket:          debug APK $PKG $POCKET_VER
Syncthing app:   Syncthing for Android $ST_VER (official syncthing/syncthing-android)
Desktop:         $(pm exec "$DESK" syncthing --version 2>/dev/null | head -1)
crun/podman:     $("${PM[@]}" --version)
elapsed:         $((SECONDS - START))s
EOF
cat "$EVID/summary.txt"
echo "PASS: LEO/NEO desktop <-> real Pocket app + real Syncthing Android (emulator) handoff"
