# netparent

A lightweight MQTT-driven MAC blocker for OpenWRT.

`netparent` runs as a daemon on an OpenWRT router (tested on Raspberry Pi 4)
and subscribes to a remote MQTT broker over TLS. It enables/disables
internet access for individual devices on the LAN by adding/removing their
MAC addresses from an `nftables` set on the `forward` chain.

Designed to be controlled by a separate web application that publishes
MQTT commands.

## Features

- Written in C with `libmosquitto`, `libuci`, and `cJSON` — small binary,
  cross-compiles cleanly to any OpenWRT target (arm, aarch64, mips,
  mipsel, x86_64, ...) via the OpenWRT SDK.
- MAC-based blocking via `nftables` — survives DHCP lease changes.
- Outbound-only TLS connection to your MQTT broker — no inbound ports
  required on the router.
- Persisted block list (survives reboots) via the UCI config.
- Last-Will-and-Testament so the broker (and your web app) always know
  the router's online state.
- procd init script + UCI configuration — feels native on OpenWRT.

## Project Layout

```
netparent/
├── src/                 C sources for the netparent daemon
├── files/               Files installed on the OpenWRT device
│   ├── etc/config/      UCI defaults
│   ├── etc/init.d/      procd init script
│   └── usr/bin/         netparentctl helper CLI
├── openwrt/Makefile     OpenWRT package Makefile (build with the SDK)
├── Makefile             Standalone Makefile (for desktop dev / testing)
├── web/                 Go admin web app (control panel for the routers)
└── docs/                Protocol + build documentation
```

The repository has two halves:

1. **Client** (root) — C daemon that runs on each OpenWRT router.
2. **Web app** (`web/`) — Go binary you run on any server. Provides a
   browser UI for the admin to view every device on every router and
   toggle internet access. See [web/README.md](web/README.md).

## Quick Start

See [docs/BUILDING.md](docs/BUILDING.md) for cross-compiling with the
OpenWRT SDK, and [docs/MQTT_PROTOCOL.md](docs/MQTT_PROTOCOL.md) for the
topic/payload contract the web app must implement.

## License

MIT
