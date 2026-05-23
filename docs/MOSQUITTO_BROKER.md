# Setting up Mosquitto for netparent

This document covers installing the Mosquitto MQTT broker on a Linux
server, configuring it for TLS-only access with client-certificate
authentication, and locking each router down to its own topic
namespace with ACLs.

It assumes you've already generated the certificates in
[MQTT_CERTIFICATES.md](MQTT_CERTIFICATES.md).

## Topology recap

```diagram
╭────────────────╮          ╭───────────────────╮          ╭──────────────────╮
│ netparent-web  │──TLS────▶│  Mosquitto broker │◀───TLS──│ netparent client │
│  user: web     │  :8883   │   (this guide)    │  :8883  │ user: router-1   │
│  cert auth     │          │                   │         │ cert auth        │
╰────────────────╯          ╰───────────────────╯         ╰──────────────────╯
```

Every connection is authenticated by **TLS client certificate** — the
cert's Common Name becomes the MQTT username, and ACLs use that
username to scope each client to its own topic prefix.

## 1. Install Mosquitto

```sh
# Debian / Ubuntu
sudo apt install mosquitto mosquitto-clients

# Arch
sudo pacman -S mosquitto

# Rocky / RHEL / Fedora
sudo dnf install mosquitto
```

The package creates a `mosquitto` system user and a systemd unit
(`mosquitto.service`) but leaves the broker disabled until configured.
Don't start it yet.

## 2. File layout

```
/etc/mosquitto/
├── mosquitto.conf              ← main config
├── conf.d/                     ← included drop-ins (we'll use this)
│   └── netparent.conf
├── certs/                      ← TLS material
│   ├── ca.crt
│   ├── broker.crt
│   └── broker.key
└── acl                         ← ACL rules
```

Create the directories and install the certificates from
[MQTT_CERTIFICATES.md](MQTT_CERTIFICATES.md):

```sh
sudo install -d -o mosquitto -g mosquitto -m 750 /etc/mosquitto/certs
sudo install -m 644 -o mosquitto -g mosquitto ca.crt     /etc/mosquitto/certs/
sudo install -m 644 -o mosquitto -g mosquitto broker.crt /etc/mosquitto/certs/
sudo install -m 600 -o mosquitto -g mosquitto broker.key /etc/mosquitto/certs/
```

## 3. Main configuration

Most distros ship a stock `/etc/mosquitto/mosquitto.conf` that just
includes everything in `conf.d/`. Don't edit the main file; put your
config in a drop-in.

```sh
sudoedit /etc/mosquitto/conf.d/netparent.conf
```

```conf
# /etc/mosquitto/conf.d/netparent.conf

# ---------- General ----------
persistence       true
persistence_location /var/lib/mosquitto/
log_dest          syslog

# Refuse anonymous clients globally.
allow_anonymous   false

# Auth + ACL files (single-file plugins, no external auth needed).
password_file     /etc/mosquitto/passwd
acl_file          /etc/mosquitto/acl

# Treat the TLS client certificate's CN as the MQTT username. This
# lets us write ACLs against per-router usernames without also
# maintaining passwords for them.
per_listener_settings true

# ---------- TLS listener (the only listener) ----------
listener 8883
protocol mqtt

cafile      /etc/mosquitto/certs/ca.crt
certfile    /etc/mosquitto/certs/broker.crt
keyfile     /etc/mosquitto/certs/broker.key

# Require every client to present a valid cert signed by our CA.
require_certificate true

# Use the cert's CN as the username (no password challenge).
use_identity_as_username true

# Modern TLS only.
tls_version          tlsv1.2

# Re-apply the listener-scoped versions of the auth settings (because
# per_listener_settings is true above, settings BEFORE the listener
# directive are ignored for that listener).
allow_anonymous   false
password_file     /etc/mosquitto/passwd
acl_file          /etc/mosquitto/acl
```

A few notes about the config:

- `require_certificate true` + `use_identity_as_username true` means
  the broker rejects any TLS client that can't present a cert from
  your CA, and uses that cert's CN as the username. **No password
  file is needed for cert-authenticated clients.**
- The `password_file` directive is still listed so that you have a
  fallback for any non-cert client you might add later. Create an
  empty file so Mosquitto doesn't complain:

  ```sh
  sudo touch /etc/mosquitto/passwd
  sudo chown mosquitto:mosquitto /etc/mosquitto/passwd
  sudo chmod 640 /etc/mosquitto/passwd
  ```

- `per_listener_settings true` is required so that
  `use_identity_as_username` is honored per-listener — without it
  Mosquitto applies global auth settings even when a listener
  overrides them.

## 4. Optional: add a password-only user

If for some reason you'd rather not give a particular client a
certificate (e.g. a quick debug script), you can fall back to plain
username/password. Drop `require_certificate true` and
`use_identity_as_username true` on a **separate** non-TLS listener,
or generate a password:

```sh
sudo mosquitto_passwd -c /etc/mosquitto/passwd alice
# (prompts twice for a password)

# Add additional users without overwriting:
sudo mosquitto_passwd /etc/mosquitto/passwd bob
```

We won't use this for netparent in production — certs are simpler and
strictly more secure.

## 5. Access Control List (ACL)

The ACL file is where you enforce that each router can only touch
its own `netparent/<device_id>/...` subtree, and that the web app can
talk to all of them.

```sh
sudoedit /etc/mosquitto/acl
```

```conf
# /etc/mosquitto/acl
#
# Mosquitto evaluates rules top-to-bottom; the first matching ACL
# decides. Anything not matched is denied.
#
# Topic patterns:
#   read   = client may subscribe & receive
#   write  = client may publish
#   readwrite = both
#
# `pattern` rules substitute %u for the authenticated username (which,
# thanks to use_identity_as_username, is the cert's CN — the router's
# device_id).

# =========================================================
# Per-router rules
# =========================================================
# Each router authenticates with cert CN == <device_id>.
# A router named "router-1" is automatically scoped to
# netparent/router-1/... — no per-router config edit needed.

# Router publishes status, devices snapshot, command responses, events.
pattern  write   netparent/%u/status
pattern  write   netparent/%u/devices
pattern  write   netparent/%u/response/+
pattern  write   netparent/%u/event/+

# Router subscribes to its own command stream.
pattern  read    netparent/%u/cmd/+

# =========================================================
# Web app (cert CN == "netparent-web")
# =========================================================
user netparent-web

# Read everything published by every router.
topic read  netparent/+/status
topic read  netparent/+/devices
topic read  netparent/+/response/+
topic read  netparent/+/event/+

# Issue commands to any router.
topic write netparent/+/cmd/+

# =========================================================
# (Optional) Admin / debug user
# =========================================================
# Uncomment and issue a "debug" client cert if you need a power user
# that can see everything:
#
# user debug
# topic readwrite netparent/#
# topic read $SYS/#
```

Set permissions:

```sh
sudo chown mosquitto:mosquitto /etc/mosquitto/acl
sudo chmod 640 /etc/mosquitto/acl
```

### How the `pattern` directive works

When `router-3` (cert CN `router-3`) connects and tries to publish to
`netparent/router-3/status`, Mosquitto substitutes `%u` with
`router-3` and the rule `pattern write netparent/%u/status` matches,
so the publish is allowed. If the same router tried to publish to
`netparent/router-7/status` it would be denied — `%u` would expand
to `router-3`, which doesn't match the topic.

This means **you do not have to edit the ACL when adding a new
router**, only issue it a cert with the right CN.

## 6. Validate, enable, and start

```sh
# Syntax-check the whole config tree.
sudo mosquitto -c /etc/mosquitto/mosquitto.conf -t   # (older versions)
# OR just attempt a foreground start:
sudo mosquitto -c /etc/mosquitto/mosquitto.conf -v

# If it stays running and prints `Opening ipv4 listen socket on port 8883.`
# you're good. Ctrl-C, then enable the service:

sudo systemctl enable --now mosquitto
sudo systemctl status mosquitto --no-pager
sudo journalctl -u mosquitto -f
```

Open the broker port (only) on the firewall:

```sh
# nftables / firewalld / ufw — pick your tool
sudo ufw allow 8883/tcp
```

## 7. Test from a client

From the host where you have the `netparent-web.crt`/`.key` pair, in
one terminal:

```sh
mosquitto_sub -h mqtt.example.com -p 8883 \
    --cafile      ./ca.crt \
    --cert        ./netparent-web.crt \
    --key         ./netparent-web.key \
    -t 'netparent/+/status' -v
```

In another, simulate a router publishing its online status:

```sh
mosquitto_pub -h mqtt.example.com -p 8883 \
    --cafile      ./ca.crt \
    --cert        ./router-1.crt \
    --key         ./router-1.key \
    -r -q 1 \
    -t 'netparent/router-1/status' \
    -m '{"online":true}'
```

The subscriber should immediately see the message. If you try the
same `mosquitto_pub` with `-t 'netparent/router-2/status'` (mismatched
device_id), the broker will silently drop it — that's the ACL working.

## 8. Operating notes

### Reload after editing the ACL or password file

```sh
sudo systemctl reload mosquitto    # SIGHUP — no client disconnects
```

### Common log lines and what they mean

| Log line                                        | Cause                                              |
| ----------------------------------------------- | -------------------------------------------------- |
| `OpenSSL Error: ... certificate verify failed`  | Client didn't send a valid cert from your CA       |
| `Socket error on client ... Protocol error`     | Client used `tcp://` instead of `tls://`           |
| `ACL denying access to ...`                     | Topic didn't match any allow rule for that user    |
| `Client connected from ... as <CN>`             | Success — note the username == cert CN             |

### Reading `$SYS/`

Mosquitto publishes broker metrics under `$SYS/broker/...`. By default
no user has access; if you want to monitor the broker, add the
relevant topics to your `debug` user (see section 5).

### Backup

The state worth backing up:

- `/etc/mosquitto/conf.d/netparent.conf`
- `/etc/mosquitto/acl`
- `/etc/mosquitto/passwd`
- `/etc/mosquitto/certs/` (the broker cert/key; the CA you keep
  separately)

Retained messages (`/var/lib/mosquitto/mosquitto.db`) are recreated
on the fly by reconnecting routers, so they don't strictly need to be
backed up.

## 9. Adding a new router — full workflow

1. Pick a `device_id`, e.g. `router-7`.
2. Issue a client cert with `CN=router-7` from your CA — see
   [MQTT_CERTIFICATES.md §3](MQTT_CERTIFICATES.md#3-create-a-client-certificate-for-each-router).
3. Copy `ca.crt`, `router-7.crt`, `router-7.key` to
   `/etc/netparent/` on the router.
4. Set `option device_id 'router-7'` in `/etc/config/netparent`.
5. `/etc/init.d/netparent restart`.

That's the entire onboarding flow — **no broker-side change needed**,
because the ACL uses `pattern ... %u` to derive the topic prefix
from the cert CN automatically.

## 10. Troubleshooting checklist

- **Client can't connect at all** → confirm port 8883 is reachable
  (`nc -zv mqtt.example.com 8883`), confirm firewall, confirm
  `listener 8883` is in the loaded config (`mosquitto -v`).
- **TLS handshake fails** → run `openssl s_client -connect host:8883
  -CAfile ca.crt -showcerts` and look at "Verify return code".
- **Publishes silently fail** → check `journalctl -u mosquitto` for
  `ACL denying access`. Verify the cert CN matches the topic prefix.
- **Web app sees no routers** → it's subscribed to
  `netparent/+/status` but the router status messages are retained, so
  the web app should get them the moment it connects. If it doesn't,
  the routers' ACLs may be blocking the publish.

For wire-level debugging, run Mosquitto in foreground with
`-v` (verbose) and watch:

```sh
sudo systemctl stop mosquitto
sudo -u mosquitto mosquitto -c /etc/mosquitto/mosquitto.conf -v
```
