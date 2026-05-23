// Package auth handles the single admin login and session cookies.
//
// Sessions are stored entirely in an HMAC-signed cookie — no server-side
// state, so the binary remains stateless and restart-safe.
package auth

import (
	"crypto/hmac"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/base64"
	"errors"
	"fmt"
	"net/http"
	"strconv"
	"strings"
	"time"

	"golang.org/x/crypto/bcrypt"
)

const cookieName = "np_session"

type Auth struct {
	username     string
	passwordHash []byte
	secret       []byte
	maxAge       time.Duration
	cookieSecure bool
}

func New(username, passwordHash, secret string, maxAge time.Duration, secure bool) *Auth {
	return &Auth{
		username:     username,
		passwordHash: []byte(passwordHash),
		secret:       []byte(secret),
		maxAge:       maxAge,
		cookieSecure: secure,
	}
}

// VerifyPassword checks credentials against the stored bcrypt hash.
func (a *Auth) VerifyPassword(username, password string) bool {
	if subtle.ConstantTimeCompare([]byte(username), []byte(a.username)) != 1 {
		// Still run bcrypt against a fake hash to avoid timing leaks.
		_ = bcrypt.CompareHashAndPassword(a.passwordHash, []byte(password))
		return false
	}
	return bcrypt.CompareHashAndPassword(a.passwordHash, []byte(password)) == nil
}

// IssueCookie writes a signed session cookie to w.
func (a *Auth) IssueCookie(w http.ResponseWriter) {
	expiry := time.Now().Add(a.maxAge).Unix()
	payload := fmt.Sprintf("%s|%d", a.username, expiry)
	sig := a.sign(payload)
	value := base64.RawURLEncoding.EncodeToString([]byte(payload)) + "." + sig

	http.SetCookie(w, &http.Cookie{
		Name:     cookieName,
		Value:    value,
		Path:     "/",
		HttpOnly: true,
		Secure:   a.cookieSecure,
		SameSite: http.SameSiteStrictMode,
		MaxAge:   int(a.maxAge / time.Second),
	})
}

// ClearCookie removes the session cookie.
func (a *Auth) ClearCookie(w http.ResponseWriter) {
	http.SetCookie(w, &http.Cookie{
		Name:     cookieName,
		Value:    "",
		Path:     "/",
		HttpOnly: true,
		Secure:   a.cookieSecure,
		SameSite: http.SameSiteStrictMode,
		MaxAge:   -1,
	})
}

// CurrentUser returns the username from a valid session cookie, or "" if none.
func (a *Auth) CurrentUser(r *http.Request) string {
	c, err := r.Cookie(cookieName)
	if err != nil || c.Value == "" {
		return ""
	}
	user, err := a.parse(c.Value)
	if err != nil {
		return ""
	}
	return user
}

func (a *Auth) sign(payload string) string {
	mac := hmac.New(sha256.New, a.secret)
	mac.Write([]byte(payload))
	return base64.RawURLEncoding.EncodeToString(mac.Sum(nil))
}

func (a *Auth) parse(value string) (string, error) {
	parts := strings.SplitN(value, ".", 2)
	if len(parts) != 2 {
		return "", errors.New("malformed cookie")
	}
	payloadBytes, err := base64.RawURLEncoding.DecodeString(parts[0])
	if err != nil {
		return "", err
	}
	expected := a.sign(string(payloadBytes))
	if subtle.ConstantTimeCompare([]byte(expected), []byte(parts[1])) != 1 {
		return "", errors.New("bad signature")
	}

	bits := strings.SplitN(string(payloadBytes), "|", 2)
	if len(bits) != 2 {
		return "", errors.New("malformed payload")
	}
	expiry, err := strconv.ParseInt(bits[1], 10, 64)
	if err != nil {
		return "", err
	}
	if time.Now().Unix() >= expiry {
		return "", errors.New("expired")
	}
	return bits[0], nil
}

// RequireLogin is HTTP middleware that redirects unauthenticated browser
// requests to /login, or returns 401 for API requests.
func (a *Auth) RequireLogin(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if a.CurrentUser(r) == "" {
			if strings.HasPrefix(r.URL.Path, "/api/") {
				http.Error(w, "unauthorized", http.StatusUnauthorized)
				return
			}
			http.Redirect(w, r, "/login", http.StatusSeeOther)
			return
		}
		next.ServeHTTP(w, r)
	})
}
