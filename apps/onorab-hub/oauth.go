package main

import (
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"errors"
	"net/http"
	"sync"
)

// OAuthGuard guarantees at most one OAuth session is in progress at a
// time (spec: "Prevent multiple plugin instances from starting multiple
// OAuth sessions"). The Hub is the single owner of OAuth; plugin
// instances never start their own sessions.
//
// Phase 1: the guard is real and tested, but no real OAuth flow runs —
// Start returns oauth_unavailable in mock mode, because there is no
// Google credential configured and no token must ever be minted from
// mock data. Phase 2 wires Start to the real PKCE flow below
// (code verifier/challenge helpers are already implemented).

// OAuthGuard is a single-flight guard for OAuth sessions.
type OAuthGuard struct {
	mu         sync.Mutex
	inProgress bool
}

func NewOAuthGuard() *OAuthGuard { return &OAuthGuard{} }

// TryStart returns true if this caller now owns the OAuth session.
// A second caller gets false and must show "OAuth already in progress".
func (g *OAuthGuard) TryStart() bool {
	g.mu.Lock()
	defer g.mu.Unlock()
	if g.inProgress {
		return false
	}
	g.inProgress = true
	return true
}

// Finish releases the session. It must be called exactly once by the
// owner, including on error paths — otherwise the Hub would block all
// future OAuth attempts.
func (g *OAuthGuard) Finish() {
	g.mu.Lock()
	defer g.mu.Unlock()
	g.inProgress = false
}

func (g *OAuthGuard) InProgress() bool {
	g.mu.Lock()
	defer g.mu.Unlock()
	return g.inProgress
}

// pkcePair generates a PKCE code verifier and its S256 challenge, per
// RFC 7636. The verifier is never logged and never sent to the plugin.
func pkcePair() (verifier, challenge string, err error) {
	b := make([]byte, 32)
	if _, err := rand.Read(b); err != nil {
		return "", "", err
	}
	verifier = base64.RawURLEncoding.EncodeToString(b)
	sum := sha256.Sum256([]byte(verifier))
	challenge = base64.RawURLEncoding.EncodeToString(sum[:])
	return verifier, challenge, nil
}

func (s *Server) handleAuthStart(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeError(w, http.StatusMethodNotAllowed, "invalid_request", "method %s not allowed on /auth/start", r.Method)
		return
	}
	if !s.oauth.TryStart() {
		writeError(w, http.StatusConflict, "oauth_in_progress", "an OAuth session is already in progress; wait for it to finish before starting another")
		return
	}
	// Phase 1 has no real OAuth session to run: release immediately and
	// report clearly, instead of pretending a login happened.
	s.oauth.Finish()
	writeError(w, http.StatusNotImplemented, "oauth_unavailable",
		"Google OAuth is not available in Phase 1 (Mock Drive Provider). "+
			"Phase 2 adds OAuth 2.0 with PKCE, drive.file scope only, with tokens in the OS keychain.")
}

// TokenStore is the Phase 2 storage boundary. Phase 1 has no tokens, so
// no implementation is exercised; the interface exists so Phase 2 does
// not invent a file-based token store under time pressure.
//
// On Windows the implementation must use Windows Credential Manager
// (wincred). A memory store is provided for tests only and must never
// be used in a shipped build.
type TokenStore interface {
	Save(account string, refreshToken string) error
	Load(account string) (string, error)
	Delete(account string) error
}

// MemoryTokenStore is test-only. See TokenStore.
type MemoryTokenStore struct {
	mu sync.Mutex
	m  map[string]string
}

func NewMemoryTokenStore() *MemoryTokenStore { return &MemoryTokenStore{m: map[string]string{}} }

func (t *MemoryTokenStore) Save(account, refreshToken string) error {
	t.mu.Lock()
	defer t.mu.Unlock()
	t.m[account] = refreshToken
	return nil
}

func (t *MemoryTokenStore) Load(account string) (string, error) {
	t.mu.Lock()
	defer t.mu.Unlock()
	v, ok := t.m[account]
	if !ok {
		return "", errors.New("no stored token for account")
	}
	return v, nil
}

func (t *MemoryTokenStore) Delete(account string) error {
	t.mu.Lock()
	defer t.mu.Unlock()
	delete(t.m, account)
	return nil
}
