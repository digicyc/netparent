# MQTT Protocol

The web application talks to one or more routers exclusively over MQTT.
All topics are namespaced by `device_id` (configured in
`/etc/config/netparent`), so a single broker can serve many routers.

## Topic map

| Topic                                            | Direction          | Retained | QoS | Purpose                                  |
| ------------------------------------------------ | ------------------ | -------- | --- | ---------------------------------------- |
| `netparent/<device_id>/cmd/<action>`             | web → router       | no       | 1   | Issue a command                          |
| `netparent/<device_id>/response/<req_id>`        | router → web       | no       | 1   | Reply for a command                      |
| `netparent/<device_id>/status`                   | router → broker    | **yes**  | 1   | `{"online":true|false}` (LWT)            |
| `netparent/<device_id>/devices`                  | router → web       | **yes**  | 1   | Full snapshot of known devices           |
| `netparent/<device_id>/event/device`             | router → web       | no       | 1   | New/changed device event                 |

## Command payloads

All command payloads are JSON. Each command may carry a `req_id`
(arbitrary string chosen by the web app) — if present, the router
publishes a reply on `netparent/<device_id>/response/<req_id>`.

### `cmd/block`

Block (deny internet for) a device by MAC.

```json
{ "mac": "aa:bb:cc:dd:ee:ff", "req_id": "evt-123" }
```

Response:

```json
{ "ok": true, "action": "block", "mac": "aa:bb:cc:dd:ee:ff" }
```

### `cmd/unblock`

```json
{ "mac": "aa:bb:cc:dd:ee:ff", "req_id": "evt-124" }
```

```json
{ "ok": true, "action": "unblock", "mac": "aa:bb:cc:dd:ee:ff" }
```

### `cmd/status`

Query whether a single MAC is currently blocked.

```json
{ "mac": "aa:bb:cc:dd:ee:ff", "req_id": "evt-125" }
```

```json
{ "ok": true, "action": "status", "blocked": "true" }
```

### `cmd/list`

List every MAC currently in the blocked set.

```json
{ "req_id": "evt-126" }
```

```json
{ "ok": true, "action": "list",
  "blocked": ["aa:bb:cc:dd:ee:ff", "11:22:33:44:55:66"] }
```

### `cmd/ping`

Health check.

```json
{ "req_id": "evt-127" }
```

```json
{ "ok": true, "action": "ping", "pong": "1" }
```

## Device discovery

The router publishes a **retained** snapshot of every device it knows
about on `netparent/<device_id>/devices`. This is the union of:

- MACs currently present in `/tmp/dhcp.leases` (active leases), and
- MACs currently in the `blocked_macs` nftables set (so blocked devices
  remain visible after their lease expires).

The payload is a JSON object:

```json
{
  "device_id": "router-1",
  "updated_at": 1716480000,
  "devices": [
    {
      "mac": "aa:bb:cc:dd:ee:ff",
      "ip": "192.168.1.42",
      "hostname": "alices-iphone",
      "blocked": false,
      "lease_expires": 1716494400,
      "last_seen": 1716480000
    },
    {
      "mac": "11:22:33:44:55:66",
      "ip": "",
      "hostname": "",
      "blocked": true,
      "lease_expires": 0,
      "last_seen": 1716470000
    }
  ]
}
```

The snapshot is republished whenever:

- the DHCP lease file changes, or
- any block/unblock command succeeds.

In addition, each individual change is announced (non-retained) on
`netparent/<device_id>/event/device`:

```json
{ "event": "added",   "device": { ...device fields... } }
{ "event": "changed", "device": { ...device fields... } }
{ "event": "removed", "mac": "aa:bb:cc:dd:ee:ff" }
```

Subscribers that only want the current state should read the retained
`devices` topic. Subscribers that want a live feed can additionally
subscribe to `event/device`.

## Online/Offline

The router publishes a retained message on
`netparent/<device_id>/status`:

- on connect: `{"online":true}`
- via MQTT LWT, on unexpected disconnect: `{"online":false}`
- on graceful shutdown: `{"online":false}`

The web app should subscribe to `netparent/+/status` to discover and
monitor all routers automatically.

## Error responses

If a command fails, `ok` is `false` and an `error` field describes the
problem:

```json
{ "ok": false, "action": "block", "error": "mac required" }
```

## Security notes

- All connections are TLS. The router validates the broker certificate
  using its configured `tls_ca_file`.
- The broker should be configured with ACLs so that each `device_id`
  can only publish/subscribe under its own `netparent/<device_id>/...`
  prefix.
- The web app should authenticate to the broker with a separate
  username/password (or client cert) from the routers.
