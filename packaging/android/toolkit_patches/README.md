# Toolkit patches carried by the Android kit

Files of the toolkit (`engine`, ps2recomp-option-p) that the APK's first-start kit carries in a newer form than the
toolkit commit the release is built from. `make_setup_assets.py --overlay <file>=<payload path>` puts the patched
file into the kit.

| Release | Patch | Payload file | Why |
|---|---|---|---|
| v0.8.5 (engine 87b590a) | `0001-translate-fork-fallback.patch` | `engine/tools/translate.py` | python.org's CPython for Android has no `_multiprocessing`: the translation's structuring runs in forked workers instead (239 s -> 123 s on a Snapdragon 8 Gen 2, output identical). Goes into the toolkit for the next release. |

To make the v0.8.5 kit's file: `git -C engine apply` the patch on 87b590a, then pass
`--overlay <the patched tools/translate.py>=engine/tools/translate.py`.
