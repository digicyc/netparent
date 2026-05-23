// Package server wires HTTP routes for the netparent admin UI and JSON API.
package server

import (
	"embed"
	"encoding/json"
	"fmt"
	"html/template"
	"io/fs"
	"log"
	"net/http"
	"regexp"
	"strings"

	"github.com/netparent/web/internal/auth"
	"github.com/netparent/web/internal/mqttbus"
	"github.com/netparent/web/internal/oui"
	"github.com/netparent/web/internal/store"
)

//go:embed all:templates all:static
var assets embed.FS

var macRegexp = regexp.MustCompile(`^([0-9a-fA-F]{2}[:-]){5}[0-9a-fA-F]{2}$`)

type Server struct {
	auth      *auth.Auth
	store     *store.Store
	bus       *mqttbus.Bus
	oui       *oui.Resolver
	templates *template.Template
}

func New(a *auth.Auth, s *store.Store, b *mqttbus.Bus, o *oui.Resolver) (*Server, error) {
	tmplFS, err := fs.Sub(assets, "templates")
	if err != nil {
		return nil, err
	}
	tmpl, err := template.ParseFS(tmplFS, "*.html")
	if err != nil {
		return nil, fmt.Errorf("parse templates: %w", err)
	}
	return &Server{
		auth:      a,
		store:     s,
		bus:       b,
		oui:       o,
		templates: tmpl,
	}, nil
}

func (s *Server) Handler() http.Handler {
	mux := http.NewServeMux()

	// Public
	mux.HandleFunc("GET /login", s.handleLoginPage)
	mux.HandleFunc("POST /login", s.handleLoginSubmit)
	mux.HandleFunc("POST /logout", s.handleLogout)

	// Static assets are public (no secrets in them).
	staticFS, _ := fs.Sub(assets, "static")
	mux.Handle("GET /static/", http.StripPrefix("/static/", http.FileServer(http.FS(staticFS))))

	// Authenticated app + API
	auth := http.NewServeMux()
	auth.HandleFunc("GET /", s.handleDashboard)
	auth.HandleFunc("GET /api/routers", s.handleAPIRouters)
	auth.HandleFunc("GET /api/routers/{id}/devices", s.handleAPIDevices)
	auth.HandleFunc("POST /api/routers/{id}/devices/{mac}/block", s.handleAPIBlock)
	auth.HandleFunc("POST /api/routers/{id}/devices/{mac}/unblock", s.handleAPIUnblock)
	mux.Handle("/", s.auth.RequireLogin(auth))

	return logMiddleware(mux)
}

// ---------- pages ----------

func (s *Server) handleLoginPage(w http.ResponseWriter, r *http.Request) {
	if s.auth.CurrentUser(r) != "" {
		http.Redirect(w, r, "/", http.StatusSeeOther)
		return
	}
	s.render(w, "login.html", map[string]any{
		"Error": r.URL.Query().Get("error"),
	})
}

func (s *Server) handleLoginSubmit(w http.ResponseWriter, r *http.Request) {
	if err := r.ParseForm(); err != nil {
		http.Error(w, "bad form", http.StatusBadRequest)
		return
	}
	user := r.PostForm.Get("username")
	pass := r.PostForm.Get("password")
	if !s.auth.VerifyPassword(user, pass) {
		http.Redirect(w, r, "/login?error=invalid", http.StatusSeeOther)
		return
	}
	s.auth.IssueCookie(w)
	http.Redirect(w, r, "/", http.StatusSeeOther)
}

func (s *Server) handleLogout(w http.ResponseWriter, r *http.Request) {
	s.auth.ClearCookie(w)
	http.Redirect(w, r, "/login", http.StatusSeeOther)
}

func (s *Server) handleDashboard(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/" {
		http.NotFound(w, r)
		return
	}
	s.render(w, "index.html", map[string]any{
		"User": s.auth.CurrentUser(r),
	})
}

// ---------- API ----------

func (s *Server) handleAPIRouters(w http.ResponseWriter, _ *http.Request) {
	writeJSON(w, http.StatusOK, map[string]any{
		"routers": s.store.Routers(),
	})
}

func (s *Server) handleAPIDevices(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	router := s.store.Router(id)
	if router == nil {
		writeJSON(w, http.StatusNotFound, map[string]string{"error": "router not found"})
		return
	}
	devices := router.DevicesList()
	// Enrich each device with its OEM. Lookup is non-blocking: it
	// returns cached value or "" and triggers async fetch on miss, so
	// vendors appear on the next poll.
	for _, d := range devices {
		d.Vendor = s.oui.Lookup(d.MAC)
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"router": map[string]any{
			"id":             router.ID,
			"online":         router.Online,
			"last_update":    router.LastUpdate,
			"devices_update": router.DevicesUpdate,
		},
		"devices": devices,
	})
}

func (s *Server) handleAPIBlock(w http.ResponseWriter, r *http.Request) {
	s.doBlockUnblock(w, r, true)
}

func (s *Server) handleAPIUnblock(w http.ResponseWriter, r *http.Request) {
	s.doBlockUnblock(w, r, false)
}

func (s *Server) doBlockUnblock(w http.ResponseWriter, r *http.Request, block bool) {
	id := r.PathValue("id")
	mac := strings.ToLower(r.PathValue("mac"))
	if !macRegexp.MatchString(mac) {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "invalid mac"})
		return
	}
	var err error
	if block {
		err = s.bus.SendBlock(id, mac)
	} else {
		err = s.bus.SendUnblock(id, mac)
	}
	if err != nil {
		writeJSON(w, http.StatusBadGateway, map[string]string{"error": err.Error()})
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"ok": true, "mac": mac})
}

// ---------- helpers ----------

func (s *Server) render(w http.ResponseWriter, name string, data any) {
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	if err := s.templates.ExecuteTemplate(w, name, data); err != nil {
		log.Printf("template %s: %v", name, err)
		http.Error(w, "template error", http.StatusInternalServerError)
	}
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}

func logMiddleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		log.Printf("%s %s", r.Method, r.URL.Path)
		next.ServeHTTP(w, r)
	})
}
