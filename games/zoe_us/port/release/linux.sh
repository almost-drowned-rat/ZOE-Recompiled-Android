# The Linux release's settings (engine/tools/linux/release_linux.sh reads them): the launcher's names - the same as
# the Windows release's zoe_launcher.exe - and the AppImage's.
LAUNCHER_GAME="Zone of the Enders"
LAUNCHER_DISC="Zone of the Enders (USA, SLUS_201.48)"
LAUNCHER_TITLE="Zone of the Enders - PC port"
LAUNCHER_EXTRA_DISC="Japanese disc (optional):"
LAUNCHER_EXTRA_KEY="disc_ja"
LAUNCHER_FILE_LABEL="Subtitles (optional):"
LAUNCHER_FILE_FILTER_NAME="Subtitle pack (*.pak)"
LAUNCHER_FILE_FILTER="*.pak"
LAUNCHER_FILE_NAME="ZOE-subtitles.pak"
APPIMAGE_NAME="ZOE-x86_64.AppImage"
# The layout (the maintainer's rule 2026-11-21): only the launcher on top - the AppImage, or the tar.gz's
# zoe_launcher - and the game built as data/zoe; an older release's top-folder programs (the game zoe_us, the
# tar.gz's launcher ZOE) are removed after the first build.
LAUNCHER_GAME_EXE="zoe"
LAUNCHER_TAR_EXE="zoe_launcher"
LAUNCHER_OLD_EXES="ZOE,zoe_us"
DESKTOP_NAME="Zone of the Enders"
DESKTOP_COMMENT="Zone of the Enders Recompiled - built from your own disc on the first start"
