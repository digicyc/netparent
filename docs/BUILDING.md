# Building netparent

**The OpenWRT SDK is the only supported build path** for shipping the
daemon. The standalone `Makefile` at the project root exists only for
quick syntax checks during development and is not viable on most
distros — `libuci` is an OpenWRT-specific library that is not packaged
for Arch, Fedora, or most other general-purpose distros, and even on
Debian/Ubuntu the daemon can't actually *run* on a workstation because
it depends on nftables policies, the OpenWRT-style UCI tree, dnsmasq
leases, and procd.

If you want to iterate on the C code without the SDK, the easiest path
is to run `gcc -fsyntax-only` against the sources (no real linking
required); see the project root's `Makefile` for the file list.

## 1. OpenWRT SDK build (for producing the package)

Pick the SDK matching your OpenWRT version and target architecture:

- Raspberry Pi 4 (default 64-bit): `bcm27xx/bcm2711` → `aarch64_cortex-a72`
- Other targets are listed at <https://downloads.openwrt.org/>.

### OpenWRT 25.12.x (apk-based)

Starting with OpenWRT 24.10 / SNAPSHOT, the package format changed from
opkg's `.ipk` to Alpine's `apk` format. The SDK build steps are the
same; only the package extension and the on-device install command
change. Example for OpenWRT 25.12.4 on a Raspberry Pi 4:

```sh
wget https://downloads.openwrt.org/releases/25.12.4/targets/bcm27xx/bcm2711/openwrt-sdk-25.12.4-bcm27xx-bcm2711_gcc-14.3.0_musl.Linux-x86_64.tar.zst
tar --zstd -xf openwrt-sdk-25.12.4-*.tar.zst
cd openwrt-sdk-25.12.4-*

# Make package feeds available. cJSON is capitalized in the feed.
./scripts/feeds update -a
./scripts/feeds install libmosquitto-ssl cJSON libuci nftables-json

# Drop netparent into the SDK as a symlink-backed package.
#
# The package Makefile at openwrt/Makefile expects to find the netparent
# checkout via a `source` symlink inside package/netparent/, so:
mkdir -p package/netparent
cp /path/to/netparent/openwrt/Makefile package/netparent/Makefile
ln -s /path/to/netparent package/netparent/source

# Build
make defconfig
make package/netparent/{clean,compile} V=s
```

The resulting package will be at:

```
bin/packages/aarch64_cortex-a72/base/netparent-0.1.0-r1.apk
```

Copy it to the router and install with `apk`:

```sh
scp bin/packages/aarch64_cortex-a72/base/netparent-*.apk root@router:/tmp/
ssh root@router 'apk add --allow-untrusted /tmp/netparent-*.apk'
```

To remove or upgrade later:

```sh
apk del netparent
apk add --allow-untrusted /tmp/netparent-<newer>.apk
```

### Older OpenWRT (23.05.x and earlier, opkg-based)

The same workflow applies, with two differences: the SDK tarball is
`.tar.xz`, and the resulting package is an `.ipk` installed with
`opkg`. Example for OpenWRT 23.05.5 on a Pi 4:

```sh
wget https://downloads.openwrt.org/releases/23.05.5/targets/bcm27xx/bcm2711/openwrt-sdk-23.05.5-bcm27xx-bcm2711_gcc-12.3.0_musl.Linux-x86_64.tar.xz
tar -xJf openwrt-sdk-23.05.5-*.tar.xz
cd openwrt-sdk-23.05.5-*

./scripts/feeds update -a
./scripts/feeds install libmosquitto-ssl cJSON libuci nftables-json

mkdir -p package/netparent
cp /path/to/netparent/openwrt/Makefile package/netparent/Makefile
ln -s /path/to/netparent package/netparent/source

make defconfig
make package/netparent/{clean,compile} V=s
```

Result:

```
bin/packages/aarch64_cortex-a72/base/netparent_0.1.0-1_aarch64_cortex-a72.ipk
```

Install with opkg:

```sh
scp bin/packages/aarch64_cortex-a72/base/netparent_*.ipk root@router:/tmp/
ssh root@router 'opkg install /tmp/netparent_*.ipk'
```

### Build gotchas

- **`cp: cannot stat './source/src'`** — the `source` symlink is
  missing or points at the wrong directory. Verify
  `ls -l package/netparent/source` resolves to your netparent checkout.
- **`Relocations in generic ELF (EM: 62) … file in wrong format`** —
  stale x86_64 `.o` files from a host build snuck into `src/`. The
  package's `Build/Prepare` step deletes them automatically, but if you
  build the standalone root `Makefile` for syntax checks, run
  `make clean` afterwards before invoking the SDK build.
- **`missing dependencies for the following libraries: libcjson.so.1`** —
  the cJSON feed package is named `cJSON` (capitalized), not
  `libcjson`. The package Makefile already uses the correct name; this
  error usually means `cJSON` wasn't installed via
  `./scripts/feeds install cJSON`.

## 2. Installation & first run

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

## 3. Cross-compiling for other architectures

The OpenWRT SDK is per-target — to build for additional architectures
(e.g. `mipsel_24kc` for older devices, `x86_64` for an x86 router),
download the matching SDK and repeat step 1. The package Makefile
itself is portable across all OpenWRT-supported targets.
