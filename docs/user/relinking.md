# Replacing the Qt libraries

Esses uses the Qt toolkit under the GNU Lesser General Public License version 3 (LGPLv3), and the Qt Advanced
Docking System library under the LGPL version 2.1. Both are linked dynamically, as separate library files. Qt is
shipped unmodified; the docking library is built from its upstream release with the few changes listed in
`third_party/qtads/UPSTREAM.md` of the Esses source. The LGPL gives you the right to run Esses with your own (for
example modified) build of these libraries. This page says where the libraries are; Help > About Esses shows the
exact Qt version of your copy and where its source code is published, and `NOTICES.md` in the installation folder
has the licence texts.

Your replacement must be binary-compatible with the version Esses was built against: the same Qt major and minor
version (a later patch release of it also works), built for the same platform with a compatible compiler (on
Windows, 64-bit Microsoft Visual C++ 2022 or later), with the same Qt modules (Core, Gui, Widgets and Svg, and their
plugins). The licence texts are in the `licenses` folder beside `NOTICES.md`.

## Windows (installer and portable zip)

The Qt libraries are the `Qt6*.dll` files beside `esses.exe`, with their plugins in the sub-folders next to them
(`platforms`, `styles`, `imageformats`, and so on); the docking library is `qtadvanceddocking-qt6.dll`. Replace those
files with your builds (keep the same file names and folder layout) and start `esses.exe`. Esses loads whatever
compatible libraries are in its folder. Reinstalling or updating Esses puts the shipped files back.

## Linux AppImage

The libraries are inside the AppImage. Extract it with `./<file>.AppImage --appimage-extract`, which creates a
`squashfs-root` folder; replace the libraries in `squashfs-root/usr/lib/` (and the plugins under
`squashfs-root/usr/plugins/`), then run `squashfs-root/AppRun`.

## macOS app

The Qt frameworks are in `Esses.app/Contents/Frameworks/` and the plugins in `Esses.app/Contents/PlugIns/`.
Replace them with your builds and start the app. The 0.1 app carries only an ad hoc signature (no developer
certificate), which a replaced framework invalidates: sign the bundle again the same way with
`codesign --force --deep --sign - Esses.app` before you start it.

## Getting the source

The source of the Qt version shipped with a release is the official Qt source archive of that version, and the
source of the docking library is the `third_party/qtads` folder of the Esses source of that release (its tagged
upstream release with the changes listed in `UPSTREAM.md` there). The About dialog and `NOTICES.md` give where to
get both.
