# MQTT Certificates

This document walks through creating the X.509 certificates that
netparent uses to secure MQTT. By the end you will have:

- A **private Certificate Authority (CA)** that signs everything else.
- A **server certificate** for the Mosquitto broker.
- A **client certificate per router** (each router authenticates with
  its own cert, and its certificate's Common Name doubles as its
  MQTT username for ACL enforcement — see
  [MOSQUITTO_BROKER.md](MOSQUITTO_BROKER.md)).
- A **client certificate for the web app**.

> **Why your own CA?** Mosquitto authenticates clients via cert chain
> validation. Issuing certs from a public CA (Let's Encrypt et al.)
> would mean anyone on the internet with any LE cert could authenticate
> to your broker. A small private CA gives you a closed trust domain
> and is essentially free to operate.

## Prerequisites

Anywhere with `openssl >= 1.1.1` (Linux, macOS, WSL):

```sh
openssl version
```

Pick a working directory; everything below assumes you `cd` into it:

```sh
mkdir -p ~/netparent-pki && cd ~/netparent-pki
```

Keep this directory **private and backed up**. Losing the CA key means
you can never sign new certificates again under the same trust chain;
leaking it lets an attacker mint client certs that the broker will
accept.

## 1. Create the Certificate Authority

The CA is a self-signed root that signs everything else. It's only
used at issuance time — clients/brokers never talk to it directly.

```sh
# 1a. Generate the CA private key (4096-bit RSA, kept on a trusted host).
openssl genrsa -out ca.key 4096
chmod 600 ca.key

# 1b. Generate the self-signed CA certificate (valid 10 years).
openssl req -x509 -new -nodes \
    -key ca.key \
    -sha256 \
    -days 3650 \
    -subj "/CN=netparent Root CA/O=netparent" \
    -out ca.crt
```

You should now have `ca.key` (secret, never leaves this host) and
`ca.crt` (public, distributed to broker + every client).

Inspect it:

```sh
openssl x509 -in ca.crt -noout -text | head -15
```

## 2. Create the broker (server) certificate

The broker presents this certificate to every connecting client. The
Subject Alternative Name (SAN) **must** include the hostname clients
use to reach it, otherwise TLS hostname verification will fail.

Set the broker hostname once:

```sh
BROKER_HOST=mqtt.example.com   # ← change me
```

Generate the broker key, a Certificate Signing Request (CSR), then
sign it with the CA:

```sh
# 2a. Broker private key.
openssl genrsa -out broker.key 4096
chmod 600 broker.key

# 2b. CSR (the broker's request to be issued a cert).
openssl req -new \
    -key broker.key \
    -subj "/CN=${BROKER_HOST}/O=netparent" \
    -out broker.csr

# 2c. Sign the CSR with the CA, embedding the SAN.
cat > broker.ext <<EOF
authorityKeyIdentifier=keyid,issuer
basicConstraints=CA:FALSE
keyUsage = digitalSignature, keyEncipherment
extendedKeyUsage = serverAuth
subjectAltName = DNS:${BROKER_HOST}
EOF

openssl x509 -req \
    -in broker.csr \
    -CA ca.crt -CAkey ca.key -CAcreateserial \
    -out broker.crt \
    -days 825 \
    -sha256 \
    -extfile broker.ext

rm broker.csr broker.ext
```

> **Tip:** If your broker is reachable by multiple names or an IP,
> repeat the `subjectAltName` line, e.g.:
> ```
> subjectAltName = DNS:${BROKER_HOST}, DNS:mqtt-internal.lan, IP:10.0.0.5
> ```

You should now have `broker.crt` and `broker.key`. These get installed
on the broker host (see [MOSQUITTO_BROKER.md](MOSQUITTO_BROKER.md)).

## 3. Create a client certificate for each router

netparent uses the cert's **Common Name as the MQTT username**, so the
CN must match the router's `device_id` in `/etc/config/netparent`.

Repeat this block once per router:

```sh
DEVICE_ID=router-1   # ← must match the router's UCI device_id

# 3a. Client private key.
openssl genrsa -out "${DEVICE_ID}.key" 4096
chmod 600 "${DEVICE_ID}.key"

# 3b. CSR.
openssl req -new \
    -key "${DEVICE_ID}.key" \
    -subj "/CN=${DEVICE_ID}/O=netparent-router" \
    -out "${DEVICE_ID}.csr"

# 3c. Sign with the CA.
cat > client.ext <<EOF
authorityKeyIdentifier=keyid,issuer
basicConstraints=CA:FALSE
keyUsage = digitalSignature
extendedKeyUsage = clientAuth
EOF

openssl x509 -req \
    -in "${DEVICE_ID}.csr" \
    -CA ca.crt -CAkey ca.key -CAcreateserial \
    -out "${DEVICE_ID}.crt" \
    -days 825 \
    -sha256 \
    -extfile client.ext

rm "${DEVICE_ID}.csr" client.ext
```

Outputs:

- `${DEVICE_ID}.crt` — install on the router as `/etc/netparent/client.crt`
- `${DEVICE_ID}.key` — install on the router as `/etc/netparent/client.key`
- `ca.crt`           — also install on the router as `/etc/netparent/ca.crt`

## 4. Create a client certificate for the web app

Same as a router, just with a distinct CN:

```sh
WEB_USER=netparent-web

openssl genrsa -out "${WEB_USER}.key" 4096
chmod 600 "${WEB_USER}.key"

openssl req -new \
    -key "${WEB_USER}.key" \
    -subj "/CN=${WEB_USER}/O=netparent-web" \
    -out "${WEB_USER}.csr"

cat > client.ext <<EOF
authorityKeyIdentifier=keyid,issuer
basicConstraints=CA:FALSE
keyUsage = digitalSignature
extendedKeyUsage = clientAuth
EOF

openssl x509 -req \
    -in "${WEB_USER}.csr" \
    -CA ca.crt -CAkey ca.key -CAcreateserial \
    -out "${WEB_USER}.crt" \
    -days 825 \
    -sha256 \
    -extfile client.ext

rm "${WEB_USER}.csr" client.ext
```

The web app's [README](../web/README.md) shows where to point the
`NETPARENT_MQTT_CA_FILE`, `NETPARENT_MQTT_CERT_FILE`, and
`NETPARENT_MQTT_KEY_FILE` env vars.

## 5. File distribution cheat-sheet

| File                | Goes to                | Path on target                       | Notes                       |
| ------------------- | ---------------------- | ------------------------------------ | --------------------------- |
| `ca.crt`            | broker + every client  | varies                               | Safe to copy widely         |
| `ca.key`            | **nowhere**            | stays on the issuing host            | Lock it down (chmod 600)    |
| `broker.crt`        | broker host            | `/etc/mosquitto/certs/broker.crt`    | Public                      |
| `broker.key`        | broker host            | `/etc/mosquitto/certs/broker.key`    | chmod 600, mosquitto user   |
| `<device_id>.crt`   | matching router        | `/etc/netparent/client.crt`          | Public                      |
| `<device_id>.key`   | matching router        | `/etc/netparent/client.key`          | chmod 600                   |
| `netparent-web.crt` | web app host           | wherever `NETPARENT_MQTT_CERT_FILE` points | Public               |
| `netparent-web.key` | web app host           | wherever `NETPARENT_MQTT_KEY_FILE` points  | chmod 600            |

Recommended secure copy:

```sh
# To a router
scp ca.crt router-1.crt router-1.key root@router-1.lan:/etc/netparent/
ssh root@router-1.lan 'chmod 600 /etc/netparent/router-1.key'

# To the broker host
scp ca.crt broker.crt broker.key admin@mqtt.example.com:/tmp/
ssh admin@mqtt.example.com '
  sudo install -m 644 -o mosquitto -g mosquitto /tmp/ca.crt     /etc/mosquitto/certs/
  sudo install -m 644 -o mosquitto -g mosquitto /tmp/broker.crt /etc/mosquitto/certs/
  sudo install -m 600 -o mosquitto -g mosquitto /tmp/broker.key /etc/mosquitto/certs/
  rm /tmp/{ca.crt,broker.crt,broker.key}
'
```

## 6. Verifying a certificate

Check the chain end-to-end:

```sh
openssl verify -CAfile ca.crt broker.crt
openssl verify -CAfile ca.crt router-1.crt
```

Both should print `OK`. Inspect a cert's fields (CN, SAN, expiry):

```sh
openssl x509 -in router-1.crt -noout -subject -issuer -dates -ext subjectAltName
```

Confirm the broker is presenting the right cert and your CA validates
it (from any client host):

```sh
openssl s_client \
    -connect mqtt.example.com:8883 \
    -CAfile ca.crt \
    -showcerts < /dev/null
# Look for: "Verify return code: 0 (ok)"
```

Test an authenticated client connection end-to-end (requires
`mosquitto-clients`):

```sh
mosquitto_sub \
    -h mqtt.example.com -p 8883 \
    --cafile ca.crt \
    --cert router-1.crt --key router-1.key \
    -t '$SYS/broker/version' -C 1
```

## 7. Renewal & revocation

- **Renewal**: client and broker certs are issued for 825 days (the
  practical maximum many TLS stacks accept). Re-run the relevant
  block in section 2/3/4 before they expire; the new cert replaces
  the old one with no broker config change.

- **Revocation**: the simplest way to revoke a router is to remove
  its line from the ACL file (see
  [MOSQUITTO_BROKER.md](MOSQUITTO_BROKER.md)) and `systemctl reload
  mosquitto`. If you need cryptographic revocation, generate a CRL:

  ```sh
  openssl ca -gencrl -keyfile ca.key -cert ca.crt -out ca.crl \
      -config /etc/ssl/openssl.cnf
  ```

  …then point `crlfile` at it in `mosquitto.conf`.

## 8. Hardening notes

- Keep `ca.key` on an offline / air-gapped host if you can.
- Rotate the CA every few years; clients only need the new CA pushed
  before the changeover.
- Don't reuse the same client cert across multiple routers — that
  breaks the per-router ACL model and makes revocation all-or-nothing.
- `chmod 600` every `.key` file and ensure they're owned by the
  process that needs them (mosquitto for the broker; root or a
  service user for clients).
