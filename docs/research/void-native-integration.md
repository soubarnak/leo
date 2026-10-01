# Void Linux integration research

Date: 2026-09-26. Scope: an initial native desktop release for the user's Void Linux x86_64/glibc machine with Wayland and niri. No Electron or embedded webview. Research for [Establish the Void packaging and desktop integration contract](https://github.com/soubarnak/leo/issues/3), assigned before this resumed investigation. Publication and ticket resolution are separate from this document; no implementation is claimed.

## Evidence and limits

Network access was restored for this resumed investigation. Context7 successfully resolved Void's handbook, xdg-desktop-portal, and libsecret. Its packaging and Email answers were incomplete, so the findings were verified against the owning projects' handbook, package templates, API definitions, and backend source through authorized shell requests. No browser retry was used. Installed manuals, library documentation, and read-only package metadata supplement those primary sources. They do not prove that an installed service is running or that this session has tested its desktop behavior.

Sources were inspected on the target environment. Local absolute paths in the source list identify evidence on this machine, rather than portable repository assets. No packages, services, settings, credentials, branches, or remote issues were changed.

## Recommendation

Make a native XBPS package the first distribution target, built against Void's native libraries with `xbps-src` and its standard CMake build style. Use the desktop toolkit's Wayland support and file dialogs, require no systemd service, and let XBPS own package updates. Treat AppImage as an optional later delivery format; the existing Electron AppImage target does not satisfy the native UI requirement. The packaging approach is supported by current upstream source; a Leo template and built package still need implementation and validation. [U1–U3]

Qt integration is locally plausible: Qt 6 base, Wayland client support, QtKeychain, libsecret, and desktop portal providers are installed. Toolkit selection and the rich-text/export proof belong to the companion stack research. Installation alone cannot establish complete feature parity.

## Locally verified platform facts

The read-only `xbps-query -l` snapshot includes the following relevant installed versions:

| Component | Observed version | What this establishes |
| --- | --- | --- |
| glibc | 2.41_1 | This environment has the requested glibc runtime |
| niri | 26.04_1 | The requested compositor package is present |
| runit / runit-void | 2.3.1_1 / 20250212_4 | Void's runit components are installed |
| Qt base / declarative | 6.11.2_1 | Native Qt libraries are present |
| Qt Wayland client | 6.11.2_1 | The Qt Wayland component is present |
| QtKeychain Qt6 | 0.17.0_1 | A native secret-storage integration library is present |
| libsecret / GNOME Keyring | 0.21.7_1 / 48.0_1 | A Secret Service client and a possible provider are present |
| xdg-desktop-portal | 1.22.1_1 | Portal frontend is installed |
| portal GNOME / GTK | 48.0_1 / 1.15.3_1 | Portal backends are installed |
| fontconfig | 2.17.1_1 | System font discovery component is present |
| Noto TTF / emoji | 2026.09.01_1 / 2.051_2 | Broad font fallback packages are present |

These are primarily runtime packages. A resumed read-only inventory confirms `cmake` 4.2.2_3, `ninja` 1.13.2_2, GCC 14.2.1+20250405_4, `pkg-config` 0.29.2_3, and `qt6-base-devel` 6.11.2_1 are installed; the earlier snapshot also shows `libsecret-devel` and `fontconfig-devel`.

Direct installed-package queries show `qtkeychain-qt6-devel`, `hunspell-devel`, `libzip-devel`, `libarchive-devel`, `qt6-svg-devel`, `qt6-wayland-devel`, and `qt6-declarative-devel` are **not installed**. For the proposed Qt Widgets route, likely additions are QtKeychain headers, Hunspell headers, and the selected ZIP/archive library headers; SVG headers are needed only if directly using Qt SVG. Choose one archive dependency. Declarative/QML headers are unnecessary for a Widgets-only UI; missing Wayland development headers do not mean the installed Qt Wayland runtime cannot run an app. Declare the actual compiled dependencies when the stack prototype establishes them. Upstream templates confirm the QtKeychain development split and native Qt6/Hunspell packaging precedent. No packages were installed. [U3, U4, L9]

The architecture reported by `uname -a` was x86_64. The sandbox's PID 1 reports `codex`, so that observation does **not** prove the host's active init process. The requested runit-compatible design and installed runit packages are the relevant evidence; do not use the sandbox process namespace to infer host service state.

## Packaging, updates, and data ownership

The installed `xbps-create(1)` manual explicitly supports building binary packages from a staged directory, with architecture, dependencies, license, version, and shared-library requirements. `xbps-rindex(1)` describes repository indexing and separate repository/package signing. `xbps-install(1)` describes installation, upgrades, and downgrades. [L1–L3]

Recommended package responsibilities:

- Install the executable, native resources, desktop entry, icons, license notices, and any required MIME metadata. Use the package's runtime dependency declarations for the chosen Qt modules and integration libraries.
- Build and test in a clean Void x86_64/glibc environment so this workstation's many development packages cannot mask missing dependencies.
- Keep manuscripts, backups, exports, preferences, and credentials outside package-owned resources. Package uninstall and upgrade must preserve user work.
- Offer version information and release notes in the app; use XBPS for package replacement. Do not let the existing Electron updater overwrite XBPS-managed files.
- Use a signed repository if distributing ongoing XBPS updates. The release publisher, signing-key custodian, and package/app identity remain decisions before publication.
- Maintain `srcpkgs/<chosen-package-name>/template` with `build_style=cmake`, a pinned release tarball/checksum, license, and version/revision. Use `hostmakedepends` for tools executed while building, `makedepends` for target headers/libraries, and explicit `depends` for plugins, external commands, and data that SONAME analysis cannot infer. The standard helper already supplies Ninja, `/usr`, and staged `DESTDIR` installation; avoid recreating its build steps. [U1–U3]

Reproducible build procedure: pin the Leo source and void-packages revision; initialize a clean x86_64/glibc `binary-bootstrap`; build with `./xbps-src pkg <chosen-package-name>`; record the resolved dependency versions and preserve their packages; inspect the result in `hostdir/binpkgs`; repeat from a fresh build container and compare artifacts. The upstream README recommends binary bootstrap and warns that full source bootstrap initially depends on the host toolchain. Pinning source alone does not freeze a rolling binary repository, and this research does not claim byte-identical builds have been achieved. [U1]

The current project already declares an Electron AppImage target for x64 and arm64, and GitHub publishing under the original upstream owner's identity. Its updater calls `electron-updater`. These are existing configuration facts, not native migration assets that can be reused unchanged. [R1, R2]

AppImage could be evaluated later for portability, but its bundling, runtime/FUSE behavior, and update implementation were not externally verified here. A native executable placed inside an AppImage would still need the same Wayland, font, mail, secret-storage, and data-integrity testing. No AppImage distribution promise should be made from this research.

## Runit, session services, and Wayland

Leo should start as a normal desktop application and require no always-running daemon. Autosave and scheduled backups can remain tied to the application lifecycle unless existing behavior proves otherwise. Do not require `systemctl`, a systemd user unit, or GNOME/KDE as the session. Void's handbook explicitly documents runit for system services. It documents Wayland seat/session requirements and `XDG_RUNTIME_DIR`; these belong to the desktop session, not a new Leo daemon. [L4, U5]

Package-name discrepancy to verify at build time: the Wayland handbook says `qt6-wayland`, whereas the inspected current `qt6-base` template packages `libqwayland.so` and Wayland integration plugins in **`qt6-wayland-client`**. Use the actual plugin-owning package for the chosen repository snapshot; a dynamically loaded platform plugin may need an explicit runtime dependency. Validate with `QT_QPA_PLATFORM=wayland` rather than assuming successful launch selected Wayland. HTTPS features likewise require the applicable Qt TLS plugin (the template includes `qt6-plugin-tls-openssl`), not just `qt6-network`. [U4, U5]

The installed niri portal configuration prefers `gnome;gtk`, routes Access and Notification to GTK, and Secret to `gnome-keyring`. The GTK backend advertises FileChooser, Print, Email, Notification, and other interfaces; the GNOME backend advertises FileChooser and Print but not Email in its installed interface list. These are backend declarations, not end-to-end functional tests. [L5]

Validate in the actual niri session: native Wayland window creation; floating/transient dialogs; fullscreen/distraction-free mode; keyboard focus and shortcuts; clipboard and drag/drop; input methods; fractional scaling; multi-monitor movement; accessibility; and file open/save cancellation. Do not silently treat successful XWayland execution as proof of native Wayland support. Check session D-Bus and portal availability at runtime and provide usable failure messages.

## Email snapshots are a concrete parity risk

Current behavior creates a timestamped PDF in `Exports`, builds a message containing a SHA-256 fingerprint, and either opens Gmail compose plus the file location or uses Apple Mail through `osascript`. The Apple Mail path is not a Linux implementation. The Gmail path already relies on manual attachment. [R3, R4]

The installed `xdg-email` help accepts `--attach`, but its implementation has client-specific handling (including Thunderbird) and a generic mailto path. Its own documentation says mailto input supports only recipient/cc/subject/body fields, with other fields ignored. Therefore opening a mailto URL or getting a zero exit status is not sufficient proof that the PDF is attached. [L6]

Recommended Linux outcome: save the snapshot durably first; request a native compose window with its PDF attachment through a verified portal/client integration; preserve the file and offer an explicit manual-attachment fallback when attachment handoff is unavailable. Preserve Gmail compose as an external-browser option if that remains part of parity. Never report that mail was sent merely because compose opened. The application should not send mail automatically.

The verified API is `org.freedesktop.portal.Email.ComposeEmail(parent_window, options)`, with `attachment_fds` as Unix file descriptors (`ah`), subject/body strings, and recipient fields. It returns a request handle; subscribe safely to `Request.Response` and distinguish success (0), cancellation (1), and other failure (2). Check the runtime interface version before using newer fields. [U6]

The specification explicitly requires the host mail client to understand attachments passed through `attachment=file://...` mailto parameters. The GTK backend constructs those parameters and reports whether `g_app_info_launch_uris` succeeded. Thus portal success establishes handoff, **not attachment acceptance or delivery**. Keep the saved export available after handoff; test Unicode/space-containing paths, absent handlers, cancellation, and the selected native mail client in the future prototype. An external-browser Gmail option remains compatible with the no-embedded-webview requirement. [U6, U7]

## Secret storage

The existing `secret:set` handler uses Electron safeStorage when available and writes plaintext values into `secrets.json` otherwise. That file is outside the manuscript library, which is good isolation, but the native port should not copy the plaintext fallback. [R5]

The installed Void QtKeychain note explicitly requires either KWallet or GNOME Keyring. Installed libsecret documentation describes a D-Bus Secret Service or secret-portal-backed encrypted file, session establishment, and unlock prompts. It also warns that lookup attributes must not contain secrets because they are not encrypted. [L7, L8]

The current freedesktop Secret Service specification independently confirms that lookup attributes are outside the secret and may be stored unencrypted, locked secrets cannot be read, unlocking may require a prompt, and previously unlocked items may be relocked at any time. Handle those errors and cancellation asynchronously; libsecret explicitly warns that synchronous lookup can block indefinitely and must not run in a UI thread. The upstream QtKeychain Void template links libsecret and provides a separate development package. [U4, U8]

Recommended invariant: store persistent API credentials in an unlocked system secret store, never in the manuscript library, backups, logs, or lookup attributes. If the store is unavailable/locked, explain the condition and allow only an explicit session-only credential, or keep the optional online feature disabled. Do not silently persist plaintext. Test absent provider, locked collection, cancelled unlock, failed write, and removal of an existing key.

Credential migration must be explicitly designed: the replacement application cannot assume it can decrypt Electron's existing encrypted values. Re-entry is a safe default; any assisted migration needs compatibility evidence and must not destroy the original credential until verified. No real credentials were inspected.

## Fonts and rendering

The current UI assumes Apple/Helvetica fonts, uses Georgia in many places, and ships cover font faces in WOFF2 plus per-family license files. PDF export currently renders HTML in a hidden Electron window. Therefore font substitution, cover layout, pagination, and PDF generation are migration risks, not packaging-only details. [R6, R7]

Recommended approach: use native system UI font selection; keep intentional document/cover font choices as document data; obtain toolkit-supported distributable font assets with their license notices; define and test fallbacks. Do not assume a WOFF2 webfont is directly loadable by the chosen native text stack or that a proprietary font is installed. Inspect licenses before converting or redistributing fonts.

Fontconfig and Noto are installed locally; Void's handbook documents Noto's language/script coverage and separate CJK/emoji packages. That does not establish identical line breaks or manuscript pagination. Golden fixtures should include the bundled cover families, Georgia/Times-style fallbacks, Latin punctuation, Bengali and other complex scripts, RTL, emoji, missing glyphs, and PDF text extraction. Required visual fidelity must be decided separately from byte-for-byte document-format compatibility. [U5]

## Smallest sufficient validation plan before release

1. Build/install a native XBPS candidate in clean Void x86_64/glibc; inspect runtime dependencies and launch it in niri with native Wayland.
2. Prove editor and export behavior using copied synthetic fixtures. Include PDF page size/margins, all export formats, and font/layout comparisons against the current application.
3. Test open/save/import dialogs, cancellations, clipboard, shortcuts, fullscreen, scaling, input methods, accessibility, and absence/failure of session integrations. Include recoverable trash for library deletion and clear failure on filesystems without trash support; never substitute permanent deletion silently. Exercise printer discovery/cancellation separately from PDF export.
4. Test email compose with one supported native client and with no configured client. Verify the actual attachment and fingerprint, then the manual fallback. Do not send test mail without separate authorization.
5. Test secret storage with available, locked, cancelled, and missing providers; assert no plaintext persistence or secrets in logs/backups.
6. Exercise autosave interruption, read-only/full disk, concurrent application opens, backup recovery, import corruption, and migration rollback on synthetic data.
7. Install an upgrade and downgrade in a disposable environment; verify package removal preserves user data. Data-schema downgrade compatibility needs its own policy rather than assuming binary downgrade is safe.

These checks are a proposed plan. No native build, portal call, mail handoff, keyring write, package installation, or migration test was performed in this research session. Native toolkit/session proof belongs to [Validate native editing and choose the stack](https://github.com/soubarnak/leo/issues/5); release installation/upgrade/downgrade/uninstall checks follow the implementation. Documentation establishes the integration contract without asserting complete runtime parity.

## Source register

- **R1:** [`package.json`](../../package.json), `build.linux`, `build.publish`, and dependencies.
- **R2:** [`main.js`](../../main.js), `checkForUpdates` near line 960.
- **R3:** [`main.js`](../../main.js), `email:draft` near line 434 and `renderPDF` near line 383.
- **R4:** [`app.js`](../../app.js), `manuscriptHash`/email flow around lines 4450–4480.
- **R5:** [`main.js`](../../main.js), `SECRETS_FILE`, `readSecret`, and `secret:set` around lines 277–305.
- **R6:** [`styles.css`](../../styles.css), default font and cover `@font-face` declarations around lines 23 and 106–117; [`fonts`](../../fonts), bundled assets and licenses.
- **R7:** [`README.md`](../../README.md), export and manuscript descriptions; [`main.js`](../../main.js), `renderPDF`.
- **L1:** Installed `/usr/share/man/man1/xbps-create.1`, description and options.
- **L2:** Installed `/usr/share/man/man1/xbps-rindex.1`, modes `--add`, `--sign`, `--sign-pkg`.
- **L3:** Installed `/usr/share/man/man1/xbps-install.1`, description.
- **L4:** Installed `/usr/share/man/man8/runit.8`, description and three stages.
- **L5:** Installed `/usr/share/xdg-desktop-portal/niri-portals.conf`, `portals/gtk.portal`, and `portals/gnome.portal`.
- **L6:** Installed `/usr/bin/xdg-email`, built-in documentation around lines 48–100, Thunderbird support around line 645, generic handler around lines 880–888; matching installed `xdg-email(1)` manual.
- **L7:** Installed `/usr/share/doc/qtkeychain-qt6/README.voidlinux`.
- **L8:** Installed `/usr/share/doc/libsecret-1/class.Service.html`, description; `libsecret-vala-examples.html`, encrypted-secret/unencrypted-attribute warning around lines 109–111.
- **L9:** Read-only `xbps-query -l` package snapshot, 2026-09-26; `uname -a` architecture observation.

Online primary sources fetched during the resumed session:

- **U1:** [void-packages README](https://github.com/void-linux/void-packages/blob/ccb8f4e0d20dca14e3313965e7da05ae75d58f88/README.md): binary bootstrap, chroot builds, output repository, and host-toolchain caveat.
- **U2:** [Packaging manual](https://github.com/void-linux/void-packages/blob/ccb8f4e0d20dca14e3313965e7da05ae75d58f88/Manual.md), dependency variables and SONAME inference; [CMake build style](https://github.com/void-linux/void-packages/blob/ccb8f4e0d20dca14e3313965e7da05ae75d58f88/common/build-style/cmake.sh).
- **U3:** [FeatherPad package template](https://github.com/void-linux/void-packages/blob/ccb8f4e0d20dca14e3313965e7da05ae75d58f88/srcpkgs/FeatherPad/template): Qt6/CMake/Hunspell precedent, not an implementation dependency.
- **U4:** [Qt6 base template](https://github.com/void-linux/void-packages/blob/ccb8f4e0d20dca14e3313965e7da05ae75d58f88/srcpkgs/qt6-base/template), especially `qt6-wayland-client_package` and TLS/development splits; [QtKeychain Qt6 template](https://github.com/void-linux/void-packages/blob/ccb8f4e0d20dca14e3313965e7da05ae75d58f88/srcpkgs/qtkeychain-qt6/template).
- **U5:** Void handbook source: [runit services](https://github.com/void-linux/void-docs/blob/master/src/config/services/index.md), [Wayland](https://github.com/void-linux/void-docs/blob/master/src/config/graphical-session/wayland.md), [fonts](https://github.com/void-linux/void-docs/blob/master/src/config/graphical-session/fonts.md). Read 2026-09-26; handbook package names must be checked against the actual build snapshot.
- **U6:** xdg-desktop-portal API definitions: [Email](https://github.com/flatpak/xdg-desktop-portal/blob/b2e6b7cf9ea66efce3ad8ee29945ed1c83261e59/data/org.freedesktop.portal.Email.xml) and [Request](https://github.com/flatpak/xdg-desktop-portal/blob/b2e6b7cf9ea66efce3ad8ee29945ed1c83261e59/data/org.freedesktop.portal.Request.xml).
- **U7:** [GTK portal Email backend](https://github.com/flatpak/xdg-desktop-portal-gtk/blob/5409af76b4514e187bd2f29033fa0fadad54a5de/src/email.c), `compose_mail_mailto` and `handle_compose_email`.
- **U8:** Freedesktop Secret Service [lookup attributes](https://specifications.freedesktop.org/secret-service/latest/lookup-attributes.html) and [locking/unlocking](https://specifications.freedesktop.org/secret-service/latest/unlocking.html); libsecret [synchronous lookup warning](https://gnome.pages.gitlab.gnome.org/libsecret/method.Service.lookup_sync.html). Read 2026-09-26.
