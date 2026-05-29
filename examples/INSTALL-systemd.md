# systemd install

This document describes how to install and run `tci-bridge` as a systemd service.

## Build and install

```bash
cmake -S . -B cmake-build-release -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build-release -j
sudo cmake --install cmake-build-release
```

By default, CMake installs under `/usr/local`, so the binary should be installed as:

```text
/usr/local/bin/tci-bridge
```

Example files are installed under:

```text
/usr/local/share/tci-bridge/examples/
```

## Install example config and service

```bash
sudo install -m 0644 /usr/local/share/tci-bridge/examples/tci-bridge.yml /etc/tci-bridge.yml
sudo install -m 0644 /usr/local/share/tci-bridge/examples/systemd/tci-bridge.service /etc/systemd/system/tci-bridge.service
```

Edit the config:

```bash
sudoedit /etc/tci-bridge.yml
```

Edit the service file:

```bash
sudoedit /etc/systemd/system/tci-bridge.service
```

## User and group settings

The example service is a normal unit named:

```text
tci-bridge.service
```

Do **not** use this in a normal, non-template unit:

```ini
User=%i
```

`%i` only makes sense in a template unit such as:

```text
tci-bridge@.service
```

For the normal unit, use explicit values:

```ini
User=rob
Group=rob
```

Adjust those values for the account that should run the bridge.

## Supplementary groups

Use only groups that exist on your system.

On Arch/Manjaro, serial devices are commonly accessible through the `uucp` group:

```ini
SupplementaryGroups=audio uucp
```

On Debian/Ubuntu, the equivalent serial-device group is often `dialout`:

```ini
SupplementaryGroups=audio dialout
```

Check available groups with:

```bash
getent group audio
getent group uucp
getent group dialout
```

If a group does not exist, do not include it in `SupplementaryGroups=`.

A conservative starting point is to comment out `SupplementaryGroups=` entirely, confirm the service starts, then add the needed groups afterward.

## Common GROUP failure

If systemd fails with something like:

```text
status=216/GROUP
Failed to determine supplementary groups
Failed at step GROUP spawning /usr/local/bin/tci-bridge
```

then one of these is wrong:

```ini
User=
Group=
SupplementaryGroups=
```

Most commonly, one of the named groups does not exist on the system, or `User=%i` was used in a non-template unit.

Check the active service file with:

```bash
systemctl cat tci-bridge.service
```

Then verify the configured user and groups:

```bash
getent passwd rob
getent group rob
getent group audio
getent group uucp
getent group dialout
```

## Enable and start

Reload systemd after installing or editing the unit:

```bash
sudo systemctl daemon-reload
```

Enable and start the service:

```bash
sudo systemctl enable --now tci-bridge.service
```

Check status:

```bash
systemctl status tci-bridge.service
```

View logs:

```bash
journalctl -u tci-bridge.service -f
```

Stop the service:

```bash
sudo systemctl stop tci-bridge.service
```

Restart the service:

```bash
sudo systemctl restart tci-bridge.service
```

## Template unit alternative

If you want a per-user template unit, name the file:

```text
tci-bridge@.service
```

Then this is valid:

```ini
User=%i
Group=%i
```

Start it as:

```bash
sudo systemctl enable --now tci-bridge@rob.service
```

For the plain `tci-bridge.service`, use explicit `User=` and `Group=` values instead.
