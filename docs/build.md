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

`cpp/install.sh` takes `--no-systemd`, and `--uninstall` removes the install.

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
both a Qt API newer than the floor and an under declared `Depends:` line.

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
