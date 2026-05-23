// Package mqttbus connects the web app to the Mosquitto broker, mirrors
// router state into the store, and exposes Publish() for outbound
// commands.
package mqttbus

import (
	"crypto/tls"
	"crypto/x509"
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"os"
	"strings"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"

	"github.com/netparent/web/internal/config"
	"github.com/netparent/web/internal/store"
)

type Bus struct {
	cfg    *config.Config
	store  *store.Store
	client mqtt.Client
}

// New creates the MQTT client but does NOT connect yet — call Connect().
func New(cfg *config.Config, st *store.Store) (*Bus, error) {
	b := &Bus{cfg: cfg, store: st}

	opts := mqtt.NewClientOptions()
	opts.AddBroker(cfg.MQTTBroker)
	opts.SetClientID(cfg.MQTTClientID)
	opts.SetCleanSession(true)
	opts.SetAutoReconnect(true)
	opts.SetMaxReconnectInterval(120 * time.Second)
	opts.SetKeepAlive(30 * time.Second)
	opts.SetConnectTimeout(10 * time.Second)
	opts.SetOrderMatters(false)

	if cfg.MQTTUsername != "" {
		opts.SetUsername(cfg.MQTTUsername)
		opts.SetPassword(cfg.MQTTPassword)
	}

	if strings.HasPrefix(cfg.MQTTBroker, "tls://") ||
		strings.HasPrefix(cfg.MQTTBroker, "ssl://") ||
		strings.HasPrefix(cfg.MQTTBroker, "mqtts://") {
		tlsConf, err := buildTLSConfig(cfg)
		if err != nil {
			return nil, err
		}
		opts.SetTLSConfig(tlsConf)
	}

	opts.SetOnConnectHandler(b.onConnect)
	opts.SetConnectionLostHandler(func(_ mqtt.Client, err error) {
		log.Printf("mqtt: connection lost: %v", err)
	})

	b.client = mqtt.NewClient(opts)
	return b, nil
}

func buildTLSConfig(cfg *config.Config) (*tls.Config, error) {
	tlsConf := &tls.Config{
		MinVersion:         tls.VersionTLS12,
		InsecureSkipVerify: cfg.MQTTInsecure,
	}

	if cfg.MQTTCAFile != "" {
		pem, err := os.ReadFile(cfg.MQTTCAFile)
		if err != nil {
			return nil, fmt.Errorf("read CA file: %w", err)
		}
		pool := x509.NewCertPool()
		if !pool.AppendCertsFromPEM(pem) {
			return nil, errors.New("CA file contained no valid certs")
		}
		tlsConf.RootCAs = pool
	}

	if cfg.MQTTCertFile != "" && cfg.MQTTKeyFile != "" {
		cert, err := tls.LoadX509KeyPair(cfg.MQTTCertFile, cfg.MQTTKeyFile)
		if err != nil {
			return nil, fmt.Errorf("load client cert/key: %w", err)
		}
		tlsConf.Certificates = []tls.Certificate{cert}
	}
	return tlsConf, nil
}

func (b *Bus) Connect() error {
	tok := b.client.Connect()
	if !tok.WaitTimeout(10 * time.Second) {
		return errors.New("mqtt connect timed out")
	}
	return tok.Error()
}

func (b *Bus) Disconnect() {
	b.client.Disconnect(500)
}

func (b *Bus) onConnect(c mqtt.Client) {
	log.Printf("mqtt: connected to %s", b.cfg.MQTTBroker)

	subs := map[string]mqtt.MessageHandler{
		"netparent/+/status":       b.handleStatus,
		"netparent/+/devices":      b.handleDevices,
		"netparent/+/event/device": b.handleDeviceEvent,
	}
	for topic, h := range subs {
		if tok := c.Subscribe(topic, 1, h); tok.Wait() && tok.Error() != nil {
			log.Printf("mqtt: subscribe %s failed: %v", topic, tok.Error())
		}
	}
}

// routerIDFromTopic extracts <id> from "netparent/<id>/..."
func routerIDFromTopic(topic string) string {
	parts := strings.Split(topic, "/")
	if len(parts) < 2 || parts[0] != "netparent" {
		return ""
	}
	return parts[1]
}

func (b *Bus) handleStatus(_ mqtt.Client, msg mqtt.Message) {
	id := routerIDFromTopic(msg.Topic())
	if id == "" {
		return
	}
	var payload struct {
		Online bool `json:"online"`
	}
	if err := json.Unmarshal(msg.Payload(), &payload); err != nil {
		log.Printf("mqtt: bad status payload on %s: %v", msg.Topic(), err)
		return
	}
	b.store.SetOnline(id, payload.Online)
	log.Printf("mqtt: router %s online=%v", id, payload.Online)
}

func (b *Bus) handleDevices(_ mqtt.Client, msg mqtt.Message) {
	id := routerIDFromTopic(msg.Topic())
	if id == "" {
		return
	}
	var payload struct {
		UpdatedAt int64           `json:"updated_at"`
		Devices   []*store.Device `json:"devices"`
	}
	if err := json.Unmarshal(msg.Payload(), &payload); err != nil {
		log.Printf("mqtt: bad devices payload on %s: %v", msg.Topic(), err)
		return
	}
	b.store.ReplaceDevices(id, payload.UpdatedAt, payload.Devices)
}

func (b *Bus) handleDeviceEvent(_ mqtt.Client, msg mqtt.Message) {
	id := routerIDFromTopic(msg.Topic())
	if id == "" {
		return
	}
	var payload struct {
		Event  string        `json:"event"`
		Device *store.Device `json:"device"`
		MAC    string        `json:"mac"`
	}
	if err := json.Unmarshal(msg.Payload(), &payload); err != nil {
		log.Printf("mqtt: bad event payload on %s: %v", msg.Topic(), err)
		return
	}
	switch payload.Event {
	case "added", "changed":
		if payload.Device != nil {
			b.store.UpsertDevice(id, payload.Device)
		}
	case "removed":
		if payload.MAC != "" {
			b.store.RemoveDevice(id, payload.MAC)
		}
	}
}

// SendBlock publishes a block command and returns immediately.
// The router's response will arrive asynchronously and update the store
// via the devices snapshot.
func (b *Bus) SendBlock(routerID, mac string) error {
	return b.publishCmd(routerID, "block", mac)
}

func (b *Bus) SendUnblock(routerID, mac string) error {
	return b.publishCmd(routerID, "unblock", mac)
}

func (b *Bus) publishCmd(routerID, action, mac string) error {
	if routerID == "" || action == "" {
		return errors.New("router and action required")
	}
	topic := fmt.Sprintf("netparent/%s/cmd/%s", routerID, action)
	body, _ := json.Marshal(map[string]string{
		"mac":    mac,
		"req_id": fmt.Sprintf("web-%d", time.Now().UnixNano()),
	})
	tok := b.client.Publish(topic, 1, false, body)
	if !tok.WaitTimeout(5 * time.Second) {
		return errors.New("publish timed out")
	}
	return tok.Error()
}
