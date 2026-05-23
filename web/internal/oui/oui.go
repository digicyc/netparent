// Package oui resolves the vendor (OEM) that owns a MAC address.
//
// Lookups use the free maclookup.app v2 API and are cached in-memory
// keyed by the OUI (first 3 octets), since every MAC sharing an OUI
// belongs to the same vendor. The cache lives for the lifetime of the
// process; restart to refresh.
//
// The resolver is non-blocking: Lookup() returns immediately with
// whatever is cached (or empty). The first observation of a new OUI
// kicks off an asynchronous fetch; the next caller (e.g. the dashboard
// poll a few seconds later) will see the populated vendor.
package oui

import (
	"context"
	"encoding/json"
	"errors"
	"log"
	"net/http"
	"strings"
	"sync"
	"time"
)

const (
	apiURL          = "https://api.maclookup.app/v2/macs/"
	fetchTimeout    = 5 * time.Second
	minFetchSpacing = 600 * time.Millisecond // stay under the free-tier 2 req/s limit
)

// sentinel value for a confirmed-unknown OUI, so we don't refetch.
const unknownMarker = "\x00unknown"

type Resolver struct {
	client *http.Client

	mu       sync.Mutex
	cache    map[string]string   // oui (lowercase, no separators) -> vendor
	inflight map[string]struct{} // oui currently being fetched

	rateMu   sync.Mutex
	lastSent time.Time
}

func New() *Resolver {
	return &Resolver{
		client:   &http.Client{Timeout: fetchTimeout},
		cache:    make(map[string]string),
		inflight: make(map[string]struct{}),
	}
}

// Lookup returns the vendor for a MAC, or "" if not yet known. It
// schedules an async fetch on miss; never blocks. Safe for concurrent
// use.
func (r *Resolver) Lookup(mac string) string {
	key, ok := ouiKey(mac)
	if !ok {
		return ""
	}

	r.mu.Lock()
	v, hit := r.cache[key]
	if !hit {
		if _, busy := r.inflight[key]; !busy {
			r.inflight[key] = struct{}{}
			go r.fetch(key, mac)
		}
	}
	r.mu.Unlock()

	if v == unknownMarker {
		return ""
	}
	return v
}

func (r *Resolver) fetch(key, mac string) {
	defer func() {
		r.mu.Lock()
		delete(r.inflight, key)
		r.mu.Unlock()
	}()

	r.respectRateLimit()

	vendor, err := r.callAPI(mac)
	r.mu.Lock()
	if err != nil {
		// Don't cache on transient errors; allow retry on next call.
		log.Printf("oui: lookup %s failed: %v", mac, err)
	} else if vendor == "" {
		r.cache[key] = unknownMarker
	} else {
		r.cache[key] = vendor
	}
	r.mu.Unlock()
}

func (r *Resolver) respectRateLimit() {
	r.rateMu.Lock()
	defer r.rateMu.Unlock()
	if d := time.Since(r.lastSent); d < minFetchSpacing {
		time.Sleep(minFetchSpacing - d)
	}
	r.lastSent = time.Now()
}

func (r *Resolver) callAPI(mac string) (string, error) {
	ctx, cancel := context.WithTimeout(context.Background(), fetchTimeout)
	defer cancel()

	req, err := http.NewRequestWithContext(ctx, http.MethodGet, apiURL+mac, nil)
	if err != nil {
		return "", err
	}
	req.Header.Set("Accept", "application/json")
	req.Header.Set("User-Agent", "netparent-web/0.1")

	resp, err := r.client.Do(req)
	if err != nil {
		return "", err
	}
	defer resp.Body.Close()

	if resp.StatusCode == http.StatusNotFound {
		return "", nil // confirmed unknown
	}
	if resp.StatusCode != http.StatusOK {
		return "", errors.New("maclookup status " + resp.Status)
	}

	var body struct {
		Success bool   `json:"success"`
		Found   bool   `json:"found"`
		Company string `json:"company"`
		IsRand  bool   `json:"isRand"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&body); err != nil {
		return "", err
	}
	if body.IsRand {
		return "(randomized MAC)", nil
	}
	if !body.Success || !body.Found {
		return "", nil
	}
	return body.Company, nil
}

// ouiKey extracts the 6-hex-char OUI prefix from a MAC string, lowercased.
// Returns ("", false) for malformed input.
func ouiKey(mac string) (string, bool) {
	var b strings.Builder
	b.Grow(6)
	for _, c := range mac {
		if c == ':' || c == '-' || c == '.' {
			continue
		}
		switch {
		case c >= '0' && c <= '9', c >= 'a' && c <= 'f':
			b.WriteRune(c)
		case c >= 'A' && c <= 'F':
			b.WriteRune(c + 32)
		default:
			return "", false
		}
		if b.Len() == 6 {
			return b.String(), true
		}
	}
	return "", false
}
