# Installation

Get current packages and checksums from the [latest release](https://github.com/dancancer/RawLab/releases/latest). Changes are listed in the [changelog](../CHANGELOG.md).

## Packages

| Platform | Package | Requirements |
| --- | --- | --- |
| Windows | [v0.7.0 x64 ZIP](https://github.com/dancancer/RawLab/releases/download/v0.7.0/RawLab-Windows-0.7.0-win-x64.zip) | Windows10/11 x64 |
| macOS Apple Silicon | [v0.7.1 arm64 ZIP (fork, Nikon HE/HE*)](https://github.com/realllllty/RawLab/releases/download/v0.7.1/RawLab-Mac-0.7.1-macOS15-arm64.zip) | macOS15+ |
| Android | [v0.7.0 signed APK](https://github.com/dancancer/RawLab/releases/download/v0.7.0/RawLab-Android-0.7.0.apk) | Android8+, ARM64 or x86_64 |
| macOS Intel | [v0.7.1 x86_64 ZIP (fork, Nikon HE/HE*)](https://github.com/realllllty/RawLab/releases/download/v0.7.1/RawLab-Mac-0.7.1-macOS15-x86_64.zip) | macOS15+ |
| iOS | [Build and sign from source](releases/v0.7.0-source.md#ios) | iOS18+; valid signing and provisioning required |

The macOS v0.7.1 packages are a release of the
[realllllty/RawLab](https://github.com/realllllty/RawLab) fork: upstream v0.7.0
plus Nikon High Efficiency (HE/HE*) NEF decoding, with
[v0.7.1 SHA-256 checksums](https://github.com/realllllty/RawLab/releases/download/v0.7.1/SHA256SUMS.txt).
The other packages remain upstream v0.7.0, covered by its
[SHA-256 checksums](https://github.com/dancancer/RawLab/releases/download/v0.7.0/SHA256SUMS.txt).
No directly installable iOS package is published for v0.7.0 or v0.7.1.

## Windows

Extract the ZIP and run `RawLab.exe`, keeping the entire directory. The package
includes .NET 8, Visual C++/OpenMP runtime libraries, LUTs and ExifTool. It is
unsigned. See the [Windows documentation](../RawLabWindows/README.md) for source
builds and [performance measurements](../RawLabWindows/performance.md).

## macOS

Choose the architecture matching your Mac and move `RawLab Mac.app` to
Applications. Apps are ad-hoc signed, not Developer ID signed or notarized;
macOS may block opening them. Control-click the app and choose Open, or run
`xattr -dr com.apple.quarantine "/Applications/RawLab Mac.app"`. The executable
and dependencies target macOS 15, but physical macOS 15 testing is still
pending. The v0.7.1 packages were built and smoke-tested on GitHub's macOS 15
arm64 and Intel runners without manual GUI testing. See the
[macOS documentation](../RawLabMac/README.md).

## Android

The APK retains the release signing identity, so it can update earlier
release-signed versions. It cannot replace a differently signed debug build.
Export any unsaved work before uninstalling a debug build. See the
[Android documentation](../RawLabAndroid/README.md).

## iOS

For step-by-step personal-device installation, see the
[iPhone installation guide](ios-installation.md)
([Simplified Chinese](ios-installation.zh-CN.md)), including an executable
workflow for AI agents.

**Unsigned IPAs cannot be installed directly.** This release has no valid Apple
signing/provisioning configuration, so its old unsigned candidate is not an
installable v0.7.0 download. Re-sign the application and
embedded framework with your own valid certificate and provisioning profile.
It is not an App Store or TestFlight package. Physical-device installation,
memory and thermal validation remain outstanding. See the
[iOS build documentation](../lutools/platform/ios/README.md).

## Source and Licenses

The release provides corresponding source, pinned dependency sources and build
instructions alongside the binaries. Original project code is distributed
under [GPLv3 only](../LICENSE); dependencies and resources retain their own
licenses. See [licensing](licensing.md) and the release notes for
[v0.7.1](releases/v0.7.1.md) and [v0.7.0](releases/v0.7.0.md).

RAW files and private signing material are never included in source packages.
