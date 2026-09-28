#!/bin/sh
set -eu

binary=$1
desktop_file=$2
expected_version=$3
version_output=$("$binary" --version)

if [ "$version_output" != "leo-writer $expected_version" ]; then
  printf 'Unexpected application identity: %s\n' "$version_output" >&2
  exit 1
fi

grep -Fxq 'Name=LEO' "$desktop_file"
grep -Fxq 'Exec=leo-writer' "$desktop_file"
grep -Fxq 'Icon=io.github.soubarnak.LeoWriter' "$desktop_file"
grep -Fxq 'StartupWMClass=io.github.soubarnak.LeoWriter' "$desktop_file"

if grep -Eq '^Exec=.*(^|[[:space:]])leo([[:space:]]|$)' "$desktop_file"; then
  printf 'Desktop launcher must not invoke the colliding leo command.\n' >&2
  exit 1
fi
