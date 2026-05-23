// netparent-web is the admin web app for controlling netparent routers
// over MQTT. Configuration is via environment variables; see config.go.
package main

import (
	"context"
	"errors"
	"log"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/netparent/web/internal/auth"
	"github.com/netparent/web/internal/config"
	"github.com/netparent/web/internal/mqttbus"
	"github.com/netparent/web/internal/oui"
	"github.com/netparent/web/internal/server"
	"github.com/netparent/web/internal/store"
)

func main() {
	log.SetFlags(log.LstdFlags | log.Lmicroseconds)

	cfg, err := config.Load()
	if err != nil {
		log.Fatalf("config: %v", err)
	}

	st := store.New()

	bus, err := mqttbus.New(cfg, st)
	if err != nil {
		log.Fatalf("mqtt: %v", err)
	}
	if err := bus.Connect(); err != nil {
		log.Fatalf("mqtt connect: %v", err)
	}
	defer bus.Disconnect()

	a := auth.New(
		cfg.AdminUsername,
		cfg.AdminPasswordHash,
		cfg.SessionSecret,
		time.Duration(cfg.SessionMaxAge)*time.Second,
		cfg.CookieSecure,
	)

	resolver := oui.New()

	srv, err := server.New(a, st, bus, resolver)
	if err != nil {
		log.Fatalf("server: %v", err)
	}

	httpSrv := &http.Server{
		Addr:              cfg.HTTPListen,
		Handler:           srv.Handler(),
		ReadHeaderTimeout: 10 * time.Second,
		ReadTimeout:       30 * time.Second,
		WriteTimeout:      30 * time.Second,
		IdleTimeout:       120 * time.Second,
	}

	// Graceful shutdown.
	stop := make(chan os.Signal, 1)
	signal.Notify(stop, syscall.SIGINT, syscall.SIGTERM)

	go func() {
		log.Printf("listening on %s", cfg.HTTPListen)
		if err := httpSrv.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
			log.Fatalf("http: %v", err)
		}
	}()

	<-stop
	log.Printf("shutting down")
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	_ = httpSrv.Shutdown(ctx)
}
