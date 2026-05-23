// Package config loads runtime configuration from environment variables.
//
// All settings are configured via env vars so the binary can be dropped
// onto any host (systemd, Docker, etc.) with no on-disk config file.
package config

import (
	"errors"
	"fmt"
	"os"
	"strconv"
)

type Config struct {
	// HTTP server
	HTTPListen string // e.g. ":8080"

	// Admin account
	AdminUsername     string
	AdminPasswordHash string // bcrypt hash

	// Session
	SessionSecret  string // HMAC key for signed cookies (>=32 bytes recommended)
	SessionMaxAge  int    // seconds
	CookieSecure   bool   // mark cookie Secure (set behind HTTPS)

	// MQTT
	MQTTBroker    string // e.g. tls://mqtt.example.com:8883
	MQTTUsername  string
	MQTTPassword  string
	MQTTClientID  string
	MQTTCAFile    string // path to CA cert (PEM)
	MQTTCertFile  string // optional client cert
	MQTTKeyFile   string // optional client key
	MQTTInsecure  bool   // skip TLS hostname verification (NOT for prod)
}

func Load() (*Config, error) {
	c := &Config{
		HTTPListen:        env("NETPARENT_HTTP_LISTEN", ":8080"),
		AdminUsername:     env("NETPARENT_ADMIN_USERNAME", "admin"),
		AdminPasswordHash: os.Getenv("NETPARENT_ADMIN_PASSWORD_HASH"),
		SessionSecret:     os.Getenv("NETPARENT_SESSION_SECRET"),
		SessionMaxAge:     envInt("NETPARENT_SESSION_MAX_AGE", 8*60*60),
		CookieSecure:      envBool("NETPARENT_COOKIE_SECURE", false),
		MQTTBroker:        env("NETPARENT_MQTT_BROKER", ""),
		MQTTUsername:      os.Getenv("NETPARENT_MQTT_USERNAME"),
		MQTTPassword:      os.Getenv("NETPARENT_MQTT_PASSWORD"),
		MQTTClientID:      env("NETPARENT_MQTT_CLIENT_ID", "netparent-web"),
		MQTTCAFile:        os.Getenv("NETPARENT_MQTT_CA_FILE"),
		MQTTCertFile:      os.Getenv("NETPARENT_MQTT_CERT_FILE"),
		MQTTKeyFile:       os.Getenv("NETPARENT_MQTT_KEY_FILE"),
		MQTTInsecure:      envBool("NETPARENT_MQTT_INSECURE", false),
	}

	var missing []string
	if c.AdminPasswordHash == "" {
		missing = append(missing, "NETPARENT_ADMIN_PASSWORD_HASH")
	}
	if c.SessionSecret == "" {
		missing = append(missing, "NETPARENT_SESSION_SECRET")
	}
	if c.MQTTBroker == "" {
		missing = append(missing, "NETPARENT_MQTT_BROKER")
	}
	if len(missing) > 0 {
		return nil, fmt.Errorf("missing required env vars: %v", missing)
	}
	if len(c.SessionSecret) < 16 {
		return nil, errors.New("NETPARENT_SESSION_SECRET must be at least 16 characters")
	}
	return c, nil
}

func env(key, fallback string) string {
	if v, ok := os.LookupEnv(key); ok {
		return v
	}
	return fallback
}

func envInt(key string, fallback int) int {
	if v, ok := os.LookupEnv(key); ok {
		if i, err := strconv.Atoi(v); err == nil {
			return i
		}
	}
	return fallback
}

func envBool(key string, fallback bool) bool {
	if v, ok := os.LookupEnv(key); ok {
		switch v {
		case "1", "true", "TRUE", "yes", "on":
			return true
		case "0", "false", "FALSE", "no", "off":
			return false
		}
	}
	return fallback
}
