// Package store holds the in-memory view of all known routers and the
// devices they report. It is updated by the MQTT bus and read by HTTP
// handlers; all access is mutex-guarded.
package store

import (
	"sort"
	"sync"
	"time"
)

type Device struct {
	MAC           string `json:"mac"`
	IP            string `json:"ip"`
	Hostname      string `json:"hostname"`
	Blocked       bool   `json:"blocked"`
	LeaseExpires  int64  `json:"lease_expires"`
	LastSeen      int64  `json:"last_seen"`
}

type Router struct {
	ID            string             `json:"id"`
	Online        bool               `json:"online"`
	LastUpdate    int64              `json:"last_update"`
	DevicesUpdate int64              `json:"devices_update"`
	Devices       map[string]*Device `json:"-"` // keyed by MAC
}

// DevicesList returns the router's devices as a stable, sorted slice.
func (r *Router) DevicesList() []*Device {
	out := make([]*Device, 0, len(r.Devices))
	for _, d := range r.Devices {
		out = append(out, d)
	}
	sort.Slice(out, func(i, j int) bool {
		// Hostnames first (alphabetical, case-insensitive), then by MAC.
		hi, hj := out[i].Hostname, out[j].Hostname
		if (hi == "") != (hj == "") {
			return hi != ""
		}
		if hi != hj {
			return hi < hj
		}
		return out[i].MAC < out[j].MAC
	})
	return out
}

type Store struct {
	mu      sync.RWMutex
	routers map[string]*Router
}

func New() *Store {
	return &Store{routers: make(map[string]*Router)}
}

// SetOnline records that we received a status message for a router.
func (s *Store) SetOnline(id string, online bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	r := s.getOrCreate(id)
	r.Online = online
	r.LastUpdate = time.Now().Unix()
}

// ReplaceDevices replaces a router's full device list (called when we
// receive the retained "devices" snapshot).
func (s *Store) ReplaceDevices(id string, updatedAt int64, devices []*Device) {
	s.mu.Lock()
	defer s.mu.Unlock()
	r := s.getOrCreate(id)
	r.Devices = make(map[string]*Device, len(devices))
	for _, d := range devices {
		r.Devices[d.MAC] = d
	}
	if updatedAt > 0 {
		r.DevicesUpdate = updatedAt
	} else {
		r.DevicesUpdate = time.Now().Unix()
	}
	r.LastUpdate = time.Now().Unix()
}

// UpsertDevice applies an "added" or "changed" event.
func (s *Store) UpsertDevice(id string, d *Device) {
	s.mu.Lock()
	defer s.mu.Unlock()
	r := s.getOrCreate(id)
	r.Devices[d.MAC] = d
	r.LastUpdate = time.Now().Unix()
}

// RemoveDevice applies a "removed" event.
func (s *Store) RemoveDevice(id, mac string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	r, ok := s.routers[id]
	if !ok {
		return
	}
	delete(r.Devices, mac)
	r.LastUpdate = time.Now().Unix()
}

// Routers returns a snapshot of all known routers (without their devices).
func (s *Store) Routers() []*Router {
	s.mu.RLock()
	defer s.mu.RUnlock()
	out := make([]*Router, 0, len(s.routers))
	for _, r := range s.routers {
		out = append(out, &Router{
			ID:            r.ID,
			Online:        r.Online,
			LastUpdate:    r.LastUpdate,
			DevicesUpdate: r.DevicesUpdate,
		})
	}
	sort.Slice(out, func(i, j int) bool { return out[i].ID < out[j].ID })
	return out
}

// Router returns a deep-copied router by ID, or nil if unknown.
func (s *Store) Router(id string) *Router {
	s.mu.RLock()
	defer s.mu.RUnlock()
	r, ok := s.routers[id]
	if !ok {
		return nil
	}
	cp := &Router{
		ID:            r.ID,
		Online:        r.Online,
		LastUpdate:    r.LastUpdate,
		DevicesUpdate: r.DevicesUpdate,
		Devices:       make(map[string]*Device, len(r.Devices)),
	}
	for k, v := range r.Devices {
		d := *v
		cp.Devices[k] = &d
	}
	return cp
}

func (s *Store) getOrCreate(id string) *Router {
	r, ok := s.routers[id]
	if !ok {
		r = &Router{ID: id, Devices: make(map[string]*Device)}
		s.routers[id] = r
	}
	return r
}
