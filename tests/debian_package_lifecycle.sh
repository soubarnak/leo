#!/bin/sh
set -eu

if [ "${LEO_PACKAGE_TEST_SANDBOX:-}" != "1" ]; then
  printf 'Run this destructive APT lifecycle check only in its disposable Debian container.\n' >&2
  exit 2
fi
if [ "$(id -u)" -ne 0 ]; then
  printf 'APT lifecycle check requires root inside the disposable container.\n' >&2
  exit 2
fi
if [ ! -e /.dockerenv ] && [ ! -e /run/.containerenv ]; then
  printf 'APT lifecycle check must run inside a disposable container.\n' >&2
  exit 2
fi

previous_package=$1
current_package=$2
test "$(dpkg-deb -f "$previous_package" Package)" = "leo-writer"
previous_version=$(dpkg-deb -f "$previous_package" Version)
test "$(dpkg-deb -f "$current_package" Package)" = "leo-writer"
current_version=$(dpkg-deb -f "$current_package" Version)
if ! dpkg --compare-versions "$current_version" gt "$previous_version"; then
  printf 'Current package version must be newer than previous package version.\n' >&2
  exit 1
fi

package_listing=$(dpkg-deb --contents "$current_package")
printf '%s\n' "$package_listing" | grep -Eq '[[:space:]]\./usr/bin/leo-writer$'
if printf '%s\n' "$package_listing" | grep -Eq '[[:space:]]\./usr/bin/leo([[:space:]]|$)'; then
  printf 'Package owns colliding /usr/bin/leo command.\n' >&2
  exit 1
fi
if printf '%s\n' "$package_listing" | grep -Eiq 'libQt[^/]*\.so|electron|node_modules'; then
  printf 'Package bundles a Qt or Electron runtime.\n' >&2
  exit 1
fi
printf '%s\n' "$(dpkg-deb -f "$current_package" Depends)" | grep -Fq 'qt6-qpa-plugins'

control_directory=$(mktemp -d)
trap 'apt-get purge -y leo-writer >/dev/null 2>&1 || true; rm -rf "$control_directory"' EXIT HUP INT TERM
dpkg-deb --control "$current_package" "$control_directory"
for script in preinst postinst prerm postrm; do
  if [ -e "$control_directory/$script" ]; then
    printf 'Unexpected package maintainer script: %s\n' "$script" >&2
    exit 1
  fi
done

apt-get install -y libwww-dict-leo-org-perl
leo_command_hash=$(sha256sum /usr/bin/leo)
leo_command_owner=$(dpkg-query -S /usr/bin/leo)

write_sentinel() {
  destination=$1
  mkdir -p "$(dirname "$destination")"
  printf 'preserve across package lifecycle\n' > "$destination"
}

assert_sentinels() {
  for sentinel in \
    "$HOME/Documents/NEO Library/package-test.txt" \
    "$HOME/.config/NEO/secrets.json" \
    "$HOME/.config/leo-writer/settings.json" \
    "$HOME/.local/share/leo-writer/snapshot.txt" \
    "$HOME/.local/state/leo-writer/session.txt" \
    "$HOME/.cache/leo-writer/cache.txt"; do
    test "$(cat "$sentinel")" = 'preserve across package lifecycle'
  done
}

write_sentinel "$HOME/Documents/NEO Library/package-test.txt"
write_sentinel "$HOME/.config/NEO/secrets.json"
write_sentinel "$HOME/.config/leo-writer/settings.json"
write_sentinel "$HOME/.local/share/leo-writer/snapshot.txt"
write_sentinel "$HOME/.local/state/leo-writer/session.txt"
write_sentinel "$HOME/.cache/leo-writer/cache.txt"

apt-get install -y "$previous_package"
previous_output=$(leo-writer --version)
case "$previous_output" in
  *" $previous_version") ;;
  *) printf 'Unexpected previous application version: %s\n' "$previous_output" >&2; exit 1 ;;
esac
assert_sentinels

apt-get install -y "$current_package"
test "$(dpkg-query -W -f='${Version}' leo-writer)" = "$current_version"
test "$(leo-writer --version)" = "leo-writer $current_version"
desktop-file-validate /usr/share/applications/io.github.soubarnak.LeoWriter.desktop
test -f /usr/share/icons/hicolor/scalable/apps/io.github.soubarnak.LeoWriter.svg
assert_sentinels
test "$(sha256sum /usr/bin/leo)" = "$leo_command_hash"
test "$(dpkg-query -S /usr/bin/leo)" = "$leo_command_owner"

apt-get remove -y leo-writer
test ! -e /usr/bin/leo-writer
test ! -e /usr/share/applications/io.github.soubarnak.LeoWriter.desktop
assert_sentinels
test "$(sha256sum /usr/bin/leo)" = "$leo_command_hash"
test "$(dpkg-query -S /usr/bin/leo)" = "$leo_command_owner"

apt-get install -y "$current_package"
test "$(dpkg-query -W -f='${Version}' leo-writer)" = "$current_version"
test "$(leo-writer --version)" = "leo-writer $current_version"
desktop-file-validate /usr/share/applications/io.github.soubarnak.LeoWriter.desktop
test -f /usr/share/icons/hicolor/scalable/apps/io.github.soubarnak.LeoWriter.svg
assert_sentinels
test "$(sha256sum /usr/bin/leo)" = "$leo_command_hash"
test "$(dpkg-query -S /usr/bin/leo)" = "$leo_command_owner"
