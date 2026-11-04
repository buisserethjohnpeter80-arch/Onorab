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

// pkceChallenge derives the S256 challenge for a verifier, per RFC
// 7636. Kept separate so tests can check it against the RFC vector.
func pkceChallenge(verifier string) string {
	sum := sha256.Sum256([]byte(verifier))
	return base64.RawURLEncoding.EncodeToString(sum[:])
}

// pkcePair generates a PKCE code verifier and its S256 challenge, per
// RFC 7636. The verifier is never logged and never sent to the plugin.
func pkcePair() (verifier, challenge string, err error) {
	b := make([]byte, 32)
	if _, err := rand.Read(b); err != nil {
		return "", "", err
	}
	verifier = base64.RawURLEncoding.EncodeToString(b)
	return verifier, pkceChallenge(verifier), nil
}

// handleAuthStart starts a Google sign-in and returns the auth URL
// for the plugin to open in the user's browser. The Hub never receives
// a password: the user signs in on Google's own page, Google redirects
// to the Hub's loopback listener, and the Hub exchanges the code for
// tokens it stores in the OS keychain. In Mock mode there is no real
// OAuth, so it reports oauth_unavailable honestly instead of faking
// a login.
func (s *Server) handleAuthStart(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeError(w, http.StatusMethodNotAllowed, "invalid_request", "method %s not allowed on /auth/start", r.Method)
		return
	}
	if !s.oauth.TryStart() {
		writeError(w, http.StatusConflict, "oauth_in_progress", "an OAuth session is already in progress; wait for it to finish before starting another")
		return
	}
	if s.driveAuth == nil {
		s.oauth.Finish()
		writeError(w, http.StatusNotImplemented, "oauth_unavailable",
			"Google OAuth is not available in Mock mode. "+
				"Start the Hub with ONORAB_DRIVE_PROVIDER=drive to use real Drive sign-in.")
		return
	}
	authURL, done, err := s.driveAuth.startBrowserSession()
	if err != nil {
		s.oauth.Finish()
		writeError(w, http.StatusInternalServerError, "internal", "could not start Google sign-in: %v", err)
		return
	}
	// The session ends asynchronously (browser callback or timeout);
	// release the single-flight guard exactly once, then.
	go func() {
		err := <-done
		s.oauth.Finish()
		if err == nil {
			s.store.events.Broadcast(OnorabEvent{Type: "auth.connected", Timestamp: nowISO()})
		}
	}()
	writeJSON(w, http.StatusOK, map[string]any{
		"status": "awaiting_user", "auth_url": authURL,
		"note": "Open this URL in a browser, sign in with your Gmail, and approve drive.file access. Then press Refresh in the plugin.",
	})
}

// handleAuthDisconnect signs the user out on this PC: the Hub revokes
// the refresh token at Google (best-effort) and deletes the token and
// the stored account from the OS keychain. After this /auth/status
// reports authenticated=false again, and the plugin shows the Connect
// button — never a stale account. In Mock mode there is nothing to
// disconnect, reported honestly.
func (s *Server) handleAuthDisconnect(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeError(w, http.StatusMethodNotAllowed, "invalid_request", "method %s not allowed on /auth/disconnect", r.Method)
		return
	}
	if s.driveAuth == nil {
		writeError(w, http.StatusNotImplemented, "oauth_unavailable",
			"Google OAuth is not available in Mock mode. "+
				"Start the Hub with ONORAB_DRIVE_PROVIDER=drive to use real Drive sign-in.")
		return
	}
	s.driveAuth.Disconnect()
	s.store.events.Broadcast(OnorabEvent{Type: "auth.disconnected", Timestamp: nowISO()})
	writeJSON(w, http.StatusOK, map[string]any{"disconnected": true})
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
