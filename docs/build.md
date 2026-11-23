# Build from source

Qt 6.2 or newer, C++20 and CMake 3.16. Install your distro's Qt 6, then build.

| Distribution | Command |
| --- | --- |
| Ubuntu / Debian / Mint / Kali | `sudo apt install build-essential cmake qt6-base-dev` |
| Fedora | `sudo dnf install gcc-c++ cmake qt6-qtbase-devel` |
| RHEL 9 / CentOS Stream 9 / Rocky / Alma | `sudo dnf install epel-release && sudo dnf config-manager --set-enabled crb && sudo dnf install gcc-c++ cmake qt6-qtbase-devel` |
| Arch / CachyOS / Manjaro | `sudo pacman -S --needed base-devel cmake qt6-base` |
| openSUSE Tumbleweed | `sudo zypper install gcc-c++ cmake qt6-base-devel` |
| Alpine | `sudo apk add build-base cmake qt6-qtbase-dev` |

On older Qt 6.2 (Ubuntu 22.04 and similar) also add `libgl-dev`. The RHEL 9
family gets Qt 6 from EPEL, as in the command above.

```bash
git clone https://github.com/codeafridi/Crowsnest.git
cd Crowsnest
cmake -S cpp -B cpp/build
cmake --build cpp/build -j"$(nproc)"
bash cpp/install.sh
```

`cpp/install.sh` installs only (nothing is started); add `--autostart` for
start-at-login, and `--uninstall` removes the install.

The tree compiles on the oldest Qt 6 the distro channel ships (6.2 on Ubuntu
22.04, 6.4 on 24.04), not only on the newest. That floor is enforced because the
package links the distribution's Qt rather than a bundled copy:

```bash
bash cpp/packaging/build_deb.sh
docker run --rm -v "$PWD":/src:ro -v /tmp/deb:/out ubuntu:24.04 \
  bash /src/cpp/packaging/ci/deb_container_build.sh
docker run --rm -v /tmp/deb:/pkg:ro -v "$PWD/cpp/packaging/ci":/ci:ro \
  ubuntu:24.04 bash /ci/deb_container_verify.sh
```

The verify script installs the package in a bare container, checks that every
shared object resolves, and runs `--dump` and `--probe`. That is the gate for
both a Qt API newer than the floor and an under declared `Depends:` line. CI
runs the same pair inside `ubuntu:22.04` and `ubuntu:24.04` containers on x86-64
and on native arm64 runners, so the arm64 package is proven the same way.

## Portable bundles and AppImages

Both self-contained formats are built inside a Rocky 9 container (EPEL Qt 6,
glibc 2.34 floor) and carry an `ARCH` stamp the installer checks:

```bash
# this machine's architecture (x86_64 or aarch64)
bash cpp/packaging/build_portable.sh
bash cpp/packaging/build_appimage.sh

# cross-build the arm64 bundle on an x86 host (qemu binfmt required)
TARGET_ARCH=aarch64 bash cpp/packaging/build_portable.sh dist-arm64

# or reuse an existing bundle for the AppImage (no Docker)
TARGET_ARCH=aarch64 bash cpp/packaging/build_appimage.sh dist-arm64
```

Output names: `crowsnest-portable.tar.gz` / `crowsnest-portable-aarch64.tar.gz`
and `Crowsnest-<version>-<arch>.AppImage` plus its `.zsync`. The installer
picks the asset for `uname -m`, refuses a bundle stamped for the other
architecture before extraction, and refuses 32-bit ARM with the arm64
requirement.

## Release provenance and signing

The release workflow signs `SHA256SUMS` with Sigstore keyless at the tag, and
the publish step refuses to run if the signature does not verify. To check a
release yourself (no key needed):

```bash
bash cpp/packaging/verify_release.sh --all v0.13.5
```

The macOS and Windows packages have optional OS signing, off until a
certificate exists:

```bash
# macOS: Developer ID Application + a notarytool keychain profile
CROWSNEST_CODESIGN_IDENTITY="Developer ID Application: ..." \
CROWSNEST_NOTARY_PROFILE="crowsnest-notary" bash cpp/packaging/build_macos.sh

# Windows: .pfx path and password
CROWSNEST_SIGN_CERT=C:\keys\crowsnest.pfx CROWSNEST_SIGN_PASSWORD=... \
  pwsh cpp/packaging/windows/build_msi.ps1 -Version 0.13.5
```

A requested signature that cannot be applied fails the build. [docs/verify.md](../verify.md)
has the exact verification commands and what each layer does and does not
prove.

## Tests

```bash
QT_QPA_PLATFORM=offscreen ctest --test-dir cpp/build --output-on-failure
```

Fourteen targets cover the parsers, the samplers, the console pages and the
platform seam. A headless crash is diagnosed with an ASan build before CI is
touched:

```bash
cmake -S cpp -B /tmp/asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS=-fsanitize=address
cmake --build /tmp/asan -j"$(nproc)"
```
