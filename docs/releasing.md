# Building and releasing TCI Bridge

TCI Bridge is a POSIX daemon built with Qt 6 and yaml-cpp. GitHub Actions
builds it on **Ubuntu 24.04 x64**, **Ubuntu 24.04 ARM64**, and **macOS Intel and Apple Silicon**.
Windows is intentionally excluded: the current Unix signal-handling code uses
`socketpair`, `sigaction`, and POSIX headers.

## Continuous integration

Pull requests and pushes to `main` build all four targets, enable
`TCI_BRIDGE_BUILD_TESTS=ON`, run both protocol test executables through CTest,
verify `--help`, `--version`, and `--check-config`, then validate each archive.

These are **hardware-free** tests. They cover TCI command/framing and a
mock rigctld backend, but cannot establish real CAT, TX/PTT safety,
audio-device compatibility, or physical-radio recovery behavior.
Run the hardware/regression tests on the Raspberry Pi appliance and radio
before creating a release tag.

## Publish a new version

The existing `v0.5.0`, `v0.5.1`, `v0.6.0`, and `v0.6.1` tags must not be reused. Choose the next unused tag (for example `v0.6.2`) after verifying all packages:

```bash
git switch main
git pull --ff-only
git tag -a v0.6.2 -m "TCI Bridge v0.6.2"
git push origin v0.6.2
```

Pushing a fresh `v*` tag rebuilds the four targets and attaches:

- `tci-bridge-linux-x64.tar.gz`
- `tci-bridge-linux-arm64.tar.gz`
- `tci-bridge-macos-x64.tar.gz`
- `tci-bridge-macos-arm64.tar.gz`
- `SHA256SUMS`

Each archive has the daemon, example YAML configuration, systemd example,
installation instructions and protocol documentation.

## Runtime requirements

These archives contain **dynamically linked binaries**, not universal or
self-contained installers. On Ubuntu 24.04, install the matching runtime
packages (Qt 6 Core, Network, WebSockets, Multimedia and yaml-cpp).
On macOS Intel or Apple Silicon, `brew install qt yaml-cpp` provides the required libraries.
For Linux ARM64, the build uses an Ubuntu 24.04 baseline; verify the Pi's
userspace glibc, Qt, and library compatibility rather than assuming the
binary works on every Raspberry Pi OS release.

Hamlib `rigctld` is a separate service and is not bundled. The daemon's
sample config has `ptt.enable_transmit: false`; enable it only after
checking the actual radio, audio I/O and TX safety behavior. Package
archives do **not** configure or enable a systemd service automatically.

## Raspberry Pi validation checklist

After extracting the ARM64 archive on a 64-bit Raspberry Pi OS system:

1. Check linked library availability using `ldd ./tci-bridge`.
2. Run `./tci-bridge --version` and `--check-config --config examples/tci-bridge.yml`.
3. Verify RX audio, CAT frequency/mode changes and reconnection using the
   configured radio and rigctld daemon.
4. With transmit explicitly enabled and a safe RF setup, verify PTT
   ownership, TX_CHRONO pacing, audio drain, watchdog and forced unkey.
5. Verify the systemd service against `examples/INSTALL-systemd.md`.

The repo's `tools/tci_smoke_test.py` can assist with websocket testing,
but its tests should run against an intentional isolated bridge instance;
they are not part of unattended release CI.
