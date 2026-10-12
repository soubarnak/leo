#!/bin/sh
# Debian release drill (#39): in a clean Debian stable container, build the
# native package from this checkout (which runs the entire test suite through
# dh_auto_test, including the parity matrix checker), rerun ctest to capture
# JUnit evidence, and verify the APT install/upgrade/remove/reinstall
# lifecycle against a previously released package.
#
# Usage:
#   scripts/debian_release_drill.sh [options]
#     --repo-dir DIR      git checkout to build (default: checkout of $0)
#     --previous-ref REF  also build REF's package and run the APT lifecycle
#                         drill against it
#     --output-dir DIR    write ctest-junit.xml and os-release there for
#                         scripts/record_parity_evidence.py (default: none)
#     --worktree          build the current working tree instead of HEAD
#                         (for trial runs before a commit; requires .git of
#                         the host checkout for --previous-ref)
#     --podman            re-execute inside a clean debian:trixie-slim
#                         container with podman (local use)
#
# Expects to run as root inside a Debian stable container (GitHub Actions
# provides that; --podman provides it locally). Exit 0 means the package
# build, the full test suite and (when requested) the package lifecycle all
# passed on Debian stable.
set -eu

repo_dir=""
previous_ref=""
output_dir=""
worktree=0
podman_mode=0

while [ $# -gt 0 ]; do
  case "$1" in
    --repo-dir) repo_dir=$2; shift 2 ;;
    --previous-ref) previous_ref=$2; shift 2 ;;
    --output-dir) output_dir=$2; shift 2 ;;
    --worktree) worktree=1; shift ;;
    --podman) podman_mode=1; shift ;;
    *) printf 'unknown option: %s\n' "$1" >&2; exit 2 ;;
  esac
done

script_path=$0
case "$script_path" in
  /*) ;;
  *) script_path=$(pwd)/$script_path ;;
esac
default_repo=$(CDPATH= cd -- "$(dirname -- "$script_path")/.." && pwd)
[ -n "$repo_dir" ] || repo_dir=$default_repo

if [ "$podman_mode" -eq 1 ]; then
  command -v podman >/dev/null || { printf 'podman not installed\n' >&2; exit 2; }
  # Podman volume mounts need absolute paths, never named-volume lookalikes.
  case "$repo_dir" in /*) ;; *) repo_dir=$(pwd)/$repo_dir ;; esac
  out=${output_dir:-$repo_dir/build/drill-out}
  case "$out" in /*) ;; *) out=$(pwd)/$out ;; esac
  mkdir -p "$out"
  args="--repo-dir /src --output-dir /out"
  [ -n "$previous_ref" ] && args="$args --previous-ref $previous_ref"
  [ "$worktree" -eq 1 ] && args="$args --worktree"
  exec podman run --rm \
    -v "$repo_dir":/src:ro -v "$out":/out:rw \
    docker.io/library/debian:trixie-slim \
    sh /src/scripts/debian_release_drill.sh $args
fi

# --- container mode -------------------------------------------------------
[ "$(id -u)" -eq 0 ] || { printf 'run as root inside the container\n' >&2; exit 2; }
[ -d "$repo_dir/.git" ] || {
  printf 'repo dir %s has no git checkout\n' "$repo_dir" >&2; exit 2
}

printf '== installing Debian build and test dependencies ==\n'
export DEBIAN_FRONTEND=noninteractive
apt-get update -q
apt-get install -y -q \
  build-essential ca-certificates cmake debhelper desktop-file-utils fakeroot git \
  qt6-base-dev qt6-qpa-plugins zlib1g-dev libpango1.0-dev libcairo2-dev pkgconf \
  nodejs python3 python3-pypdf python3-pdfminer \
  hunspell-en-us libhunspell-1.7-0 \
  fonts-dejavu-core fonts-noto-core fonts-noto-color-emoji \
  libreoffice-writer libwww-dict-leo-org-perl

# Test dependencies are guaranteed, so no suite member can pass by skipping
# itself for a missing tool (NEO round trip needs node, export checks need
# python readers and LibreOffice, spellcheck needs the en_US dictionary).
printf '== asserting test toolchain ==\n'
command -v node >/dev/null
command -v python3 >/dev/null
python3 -c 'import pypdf, pdfminer'
command -v soffice >/dev/null
test -f /usr/share/hunspell/en_US.dic
command -v desktop-file-validate >/dev/null
command -v dpkg-buildpackage >/dev/null

git config --global --add safe.directory "$repo_dir"

work=$(mktemp -d)
printf '== preparing source trees in %s ==\n' "$work"
mkdir -p "$work/cur"
if [ "$worktree" -eq 1 ]; then
  # Trial mode: snapshot the working tree (including uncommitted work).
  tar -C "$repo_dir" \
    --exclude=./.git --exclude=./build --exclude=./node_modules \
    --exclude=./.commandcode --exclude=./.pi --exclude=./'Claude outputs' \
    -cf - . | tar -xf - -C "$work/cur"
else
  git clone -q "$repo_dir" "$work/cur"
  # debhelper drops untracked build files (debian/files, obj-*); the parity
  # checker verifies tracked build paths against the build commit instead.
  git -C "$work/cur" config status.showUntrackedFiles no
fi
version=$(sed -n '1s/^[^ ]* (\([^)]*\)).*/\1/p' "$work/cur/debian/changelog")
printf '== building leo-writer %s ==\n' "$version"
(cd "$work/cur" && dpkg-buildpackage -us -uc -b)
current_deb="$work/leo-writer_${version}_amd64.deb"
[ -f "$current_deb" ] || { printf 'missing %s\n' "$current_deb" >&2; exit 2; }

if [ -n "$previous_ref" ]; then
  printf '== building previous package from %s ==\n' "$previous_ref"
  mkdir -p "$work/prev"
  git -C "$repo_dir" archive "$previous_ref" | tar -x -C "$work/prev"
  previous_version=$(sed -n '1s/^[^ ]* (\([^)]*\)).*/\1/p' "$work/prev/debian/changelog")
  dpkg --compare-versions "$version" gt "$previous_version" || {
    printf 'version %s is not newer than %s\n' "$version" "$previous_version" >&2
    exit 2
  }
  # Only the previous *package* matters for the lifecycle drill; its own
  # test suite is the previous release's business, not this build's.
  (cd "$work/prev" && DEB_BUILD_OPTIONS=nocheck dpkg-buildpackage -us -uc -b)
  previous_deb="$work/leo-writer_${previous_version}_amd64.deb"
  [ -f "$previous_deb" ] || { printf 'missing %s\n' "$previous_deb" >&2; exit 2; }

  printf '== APT lifecycle drill (install, upgrade, remove, reinstall) ==\n'
  LEO_PACKAGE_TEST_SANDBOX=1 sh "$work/cur/tests/debian_package_lifecycle.sh" \
    "$previous_deb" "$current_deb"
fi

printf '== recording ctest evidence ==\n'
if [ -n "$output_dir" ]; then
  mkdir -p "$output_dir"
  (cd "$work/cur/obj-x86_64-linux-gnu" && \
    ctest -j"$(nproc)" --output-on-failure -E '^leo-parity-matrix$' \
      --output-junit "$output_dir/ctest-junit.xml")
  cp /etc/os-release "$output_dir/os-release"
else
  (cd "$work/cur/obj-x86_64-linux-gnu" && \
    ctest -j"$(nproc)" --output-on-failure -E '^leo-parity-matrix$')
fi

printf '== Debian release drill passed: build, tests%s on Debian stable ==\n' \
  "${previous_ref:+, APT lifecycle}"
