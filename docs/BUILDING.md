# Building netparent

There are two supported build flows: a desktop build for development
and an OpenWRT SDK build for producing installable `.ipk` packages.

## 1. Desktop build (for development)

On a Debian/Ubuntu/Arch machine, install dependencies:

```sh
# Debian / Ubuntu
sudo apt install build-essential pkg-config \
    libmosquitto-dev libcjson-dev libuci-dev nftables

# Arch
sudo pacman -S base-devel pkgconf mosquitto cjson libuci nftables
```

Build:

```sh
make
```

This produces `./netparent`. It expects to read its config from UCI
(`/etc/config/netparent`), so the easiest way to smoke-test is to
create a minimal config there or run it inside a VM that already has
an OpenWRT-style UCI layout. For a quick sanity check that just
exercises the MQTT/JSON path, point `tls_enabled '0'` at a local
broker.

## 2. OpenWRT SDK build (for producing .ipk)

Pick the SDK matching your OpenWRT version and target architecture:

- Raspberry Pi 4 (default 64-bit): `bcm27xx/bcm2711` → `aarch64_cortex-a72`
- Other targets are listed at <https://downloads.openwrt.org/>.

```sh
# Example for OpenWRT 23.05 on Pi 4
wget https://downloads.openwrt.org/releases/23.05.5/targets/bcm27xx/bcm2711/openwrt-sdk-23.05.5-bcm27xx-bcm2711_gcc-12.3.0_musl.Linux-x86_64.tar.xz
tar -xJf openwrt-sdk-*.tar.xz
cd openwrt-sdk-*

# Make package feeds available
./scripts/feeds update -a
./scripts/feeds install libmosquitto-ssl libcjson libuci nftables-json

# Drop netparent into the SDK
mkdir -p package/netparent
cp -r /path/to/this/repo/openwrt/* package/netparent/
# The OpenWRT Makefile references ../src and ../files relative to
# package/netparent/, so also copy or symlink the project tree alongside:
ln -s /path/to/this/repo package/netparent/source

# Or simpler: copy the entire project so paths line up
cp -r /path/to/this/repo/* package/netparent/

# Build
make package/netparent/{clean,compile} V=s
```

The resulting `.ipk` will be at:
`bin/packages/aarch64_cortex-a72/base/netparent_0.1.0-1_aarch64_cortex-a72.ipk`

Copy it to the router and install:

```sh
scp netparent_*.ipk root@router:/tmp/
ssh root@router 'opkg install /tmp/netparent_*.ipk'
```

## 3. Installation & first run

1. Place your broker CA certificate at `/etc/netparent/ca.crt`
   (and the optional client cert/key if you use mutual TLS).
2. Edit `/etc/config/netparent` — set `device_id`, `broker_host`,
   credentials, etc.
3. Start the service:
   ```sh
   /etc/init.d/netparent enable
   /etc/init.d/netparent start
   logread -e netparent -f
   ```
4. From any MQTT client, publish a test:
   ```sh
   mosquitto_pub -h mqtt.example.com -p 8883 --cafile ca.crt \
     -t 'netparent/router-1/cmd/ping' \
     -m '{"req_id":"hello"}'
   ```

## 4. Cross-compiling for other architectures

The OpenWRT SDK is per-target — to build for additional architectures
(e.g. `mipsel_24kc` for older devices, `x86_64` for an x86 router),
download the matching SDK and repeat step 2. The package Makefile
itself is portable across all OpenWRT-supported targets.
