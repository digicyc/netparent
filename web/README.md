# netparent-web

Go-based admin web app for controlling [netparent](../README.md) routers
over MQTT.

- Single admin user (no multi-user system; bcrypt password hash)
- Session via HMAC-signed cookie (no server-side state)
- Lists every router that has ever published a status message
- For each router, lists every device (enabled/disabled) and toggles
  internet access with one click
- Each device is annotated with its **OEM / vendor**, resolved from the
  MAC's OUI via [maclookup.app](https://maclookup.app) and cached
  in-memory (one network call per unique vendor, ever)
- All MQTT traffic goes through the broker that the routers also use

## Build

```sh
make tidy        # fetch deps (first time)
make             # produces ./netparent-web and ./hashpw
```

Or with raw Go:

```sh
go build -o netparent-web ./cmd/netparent-web
go build -o hashpw        ./cmd/hashpw
```

## Generate an admin password hash

```sh
echo -n 'mySecretPassword' | ./hashpw
# outputs: $2a$10$abcdef.....
```

Copy that into `NETPARENT_ADMIN_PASSWORD_HASH`.

## Configuration (environment variables)

| Variable                             | Required | Default            | Description                                      |
| ------------------------------------ | -------- | ------------------ | ------------------------------------------------ |
| `NETPARENT_HTTP_LISTEN`              |          | `:8080`            | HTTP bind address                                |
| `NETPARENT_ADMIN_USERNAME`           |          | `admin`            | Login username                                   |
| `NETPARENT_ADMIN_PASSWORD_HASH`      | **yes**  | —                  | bcrypt hash of admin password                    |
| `NETPARENT_SESSION_SECRET`           | **yes**  | —                  | ≥16 chars, HMAC key for session cookies          |
| `NETPARENT_SESSION_MAX_AGE`          |          | `28800`            | Session lifetime in seconds                      |
| `NETPARENT_COOKIE_SECURE`            |          | `false`            | Set to `1` when serving over HTTPS               |
| `NETPARENT_MQTT_BROKER`              | **yes**  | —                  | e.g. `tls://mqtt.example.com:8883`               |
| `NETPARENT_MQTT_USERNAME`            |          | —                  | MQTT username                                    |
| `NETPARENT_MQTT_PASSWORD`            |          | —                  | MQTT password                                    |
| `NETPARENT_MQTT_CLIENT_ID`           |          | `netparent-web`    | MQTT client ID                                   |
| `NETPARENT_MQTT_CA_FILE`             |          | —                  | Broker CA cert (PEM)                             |
| `NETPARENT_MQTT_CERT_FILE`           |          | —                  | Client cert (mutual TLS, optional)               |
| `NETPARENT_MQTT_KEY_FILE`            |          | —                  | Client key (mutual TLS, optional)                |
| `NETPARENT_MQTT_INSECURE`            |          | `false`            | Skip TLS hostname verification (NOT for prod)    |

## Run

```sh
export NETPARENT_ADMIN_PASSWORD_HASH='$2a$10$...'
export NETPARENT_SESSION_SECRET="$(openssl rand -base64 32)"
export NETPARENT_MQTT_BROKER='tls://mqtt.example.com:8883'
export NETPARENT_MQTT_USERNAME='web'
export NETPARENT_MQTT_PASSWORD='change-me'
export NETPARENT_MQTT_CA_FILE='./ca.crt'

./netparent-web
```

Browse to <http://localhost:8080/login>.

## Deploy behind HTTPS

Put `netparent-web` behind a TLS-terminating reverse proxy (nginx,
Caddy, Traefik) and set `NETPARENT_COOKIE_SECURE=1`. The app speaks
plain HTTP itself; the proxy handles certificates.

### nginx

Install nginx and (for free TLS certs) certbot:

```sh
# Debian / Ubuntu
sudo apt install nginx certbot python3-certbot-nginx

# Arch
sudo pacman -S nginx certbot certbot-nginx
```

Create `/etc/nginx/sites-available/netparent-web` (on Debian/Ubuntu)
or `/etc/nginx/conf.d/netparent-web.conf` (on Arch/RHEL):

```nginx
# HTTP → HTTPS redirect
server {
    listen 80;
    listen [::]:80;
    server_name admin.example.com;

    # Allow Let's Encrypt HTTP-01 challenges through.
    location /.well-known/acme-challenge/ {
        root /var/www/html;
    }

    location / {
        return 301 https://$host$request_uri;
    }
}

# Main HTTPS server
server {
    listen 443 ssl http2;
    listen [::]:443 ssl http2;
    server_name admin.example.com;

    # Certificate (filled in by certbot, or supply your own).
    ssl_certificate     /etc/letsencrypt/live/admin.example.com/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/admin.example.com/privkey.pem;

    # Reasonable TLS defaults.
    ssl_protocols       TLSv1.2 TLSv1.3;
    ssl_ciphers         HIGH:!aNULL:!MD5;
    ssl_prefer_server_ciphers on;
    ssl_session_cache   shared:SSL:10m;
    ssl_session_timeout 1h;

    # Security headers.
    add_header Strict-Transport-Security "max-age=31536000; includeSubDomains" always;
    add_header X-Content-Type-Options    "nosniff"                              always;
    add_header X-Frame-Options           "DENY"                                 always;
    add_header Referrer-Policy           "strict-origin-when-cross-origin"     always;

    # Modest upload cap — the API only sends tiny JSON bodies.
    client_max_body_size 1m;

    location / {
        proxy_pass         http://127.0.0.1:8080;
        proxy_http_version 1.1;

        proxy_set_header   Host              $host;
        proxy_set_header   X-Real-IP         $remote_addr;
        proxy_set_header   X-Forwarded-For   $proxy_add_x_forwarded_for;
        proxy_set_header   X-Forwarded-Proto $scheme;
        proxy_set_header   X-Forwarded-Host  $host;

        proxy_connect_timeout 10s;
        proxy_read_timeout    60s;
        proxy_send_timeout    60s;
    }
}
```

Enable, test, and reload:

```sh
# Debian/Ubuntu only — Arch can skip the symlink step.
sudo ln -s /etc/nginx/sites-available/netparent-web \
          /etc/nginx/sites-enabled/netparent-web

sudo nginx -t                 # validate config
sudo systemctl reload nginx
```

Obtain a Let's Encrypt certificate (run once; certbot adds a renewal
timer automatically):

```sh
sudo certbot --nginx -d admin.example.com
```

Finally, run `netparent-web` bound to **localhost only** and with the
secure-cookie flag on so the session cookie is never sent over plain
HTTP:

```sh
export NETPARENT_HTTP_LISTEN='127.0.0.1:8080'
export NETPARENT_COOKIE_SECURE=1
./netparent-web
```

In production you'll typically run it under systemd. A minimal unit
file (`/etc/systemd/system/netparent-web.service`):

```ini
[Unit]
Description=netparent admin web app
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=netparent
EnvironmentFile=/etc/netparent-web/env
ExecStart=/usr/local/bin/netparent-web
Restart=on-failure
RestartSec=5s

# Hardening
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true
PrivateTmp=true
ReadOnlyPaths=/etc/netparent-web

[Install]
WantedBy=multi-user.target
```

Put every `NETPARENT_*` variable in `/etc/netparent-web/env` (one
`KEY=value` per line, no `export`), then:

```sh
sudo useradd --system --no-create-home --shell /usr/sbin/nologin netparent
sudo install -m 755 ./netparent-web /usr/local/bin/
sudo systemctl daemon-reload
sudo systemctl enable --now netparent-web
sudo journalctl -u netparent-web -f
```

### Caddy (alternative)

If you'd rather use Caddy (auto-HTTPS, smaller config):

```caddy
admin.example.com {
    reverse_proxy localhost:8080
}
```

## How it talks to the routers

Subscribes (at startup) to:
- `netparent/+/status`
- `netparent/+/devices`
- `netparent/+/event/device`

…and maintains an in-memory view of every router and its devices.

Publishes (on click):
- `netparent/<id>/cmd/block`
- `netparent/<id>/cmd/unblock`

The full protocol contract lives in
[../docs/MQTT_PROTOCOL.md](../docs/MQTT_PROTOCOL.md).
