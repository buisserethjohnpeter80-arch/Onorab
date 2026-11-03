package main

import (
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"net/http"
	"net/url"
	"os"
	"strings"
	"sync"
	"time"
)

// Real Google Drive OAuth for the commercial, multi-PC model.
//
// Correction to the Phase 1 single-PC assumption (user, 2026-11-03):
// the VST will be sold and installed on many PCs, so the OAuth client
// is NOT a per-PC secret. It is Onorab's app identity, shared by every
// installation, and every buyer signs in with THEIR OWN Gmail on THEIR
// OWN PC. What stays per-PC is the USER's token, stored in that PC's
// OS keychain (Windows Credential Manager) — never in a repo, never
// in an FL project, never sent to the plugin or to Muse.
//
// Client identity resolution (in order):
//  1. ONORAB_OAUTH_CLIENT_ID environment variable (dev / manual runs).
//  2. buildOAuthClientID, injected at release-build time by CI from a
//     GitHub Actions secret (see docs/architecture.md §3.4). The repo
//     itself contains NO client id and NO client secret.
//  3. A Desktop OAuth JSON file at ONORAB_CREDENTIALS_PATH (dev only,
//     e.g. the file downloaded from Google Cloud Console). This path
//     is for the developer's own testing; shipped builds use (1)/(2).
//
// For Desktop (installed) apps with PKCE, the token exchange does not
// require a client secret — that is the point of PKCE for apps that
// cannot keep a secret. If a client secret IS configured (dev file),
// it is sent too; shipped builds send client_id + PKCE only, so there
// is no secret inside the distributed binary to extract.
//
// Scope is drive.file ONLY: the app sees files it created, plus files
// explicitly shared with the signed-in user for Onorab projects —
// never the user's whole Drive.

// buildOAuthClientID is set by the release build:
//   go build -ldflags "-X main.buildOAuthClientID=<client_id>"
// It is empty in plain `go build` / `go test` runs.
var buildOAuthClientID string

const (
	googleAuthURL  = "https://accounts.google.com/o/oauth2/v2/auth"
	googleTokenURL = "https://oauth2.googleapis.com/token"
	driveFileScope = "https://www.googleapis.com/auth/drive.file"
)

// resolveOAuthClient returns (clientID, clientSecret, error) following
// the precedence above. The secret is "" when only an id is configured.
func resolveOAuthClient(credentialsPath string) (string, string, error) {
	if id := strings.TrimSpace(os.Getenv("ONORAB_OAUTH_CLIENT_ID")); id != "" {
		return id, strings.TrimSpace(os.Getenv("ONORAB_OAUTH_CLIENT_SECRET")), nil
	}
	if id := strings.TrimSpace(buildOAuthClientID); id != "" {
		return id, "", nil
	}
	if credentialsPath != "" {
		data, err := os.ReadFile(credentialsPath)
		if err != nil {
			return "", "", fmt.Errorf("read OAuth credentials file: %w", err)
		}
		var doc struct {
			Installed struct {
				ClientID     string `json:"client_id"`
				ClientSecret string `json:"client_secret"`
			} `json:"installed"`
			ClientID     string `json:"client_id"`
			ClientSecret string `json:"client_secret"`
		}
		if err := json.Unmarshal(data, &doc); err != nil {
			return "", "", fmt.Errorf("parse OAuth credentials file: %w", err)
		}
		if doc.Installed.ClientID != "" {
			return doc.Installed.ClientID, doc.Installed.ClientSecret, nil
		}
		if doc.ClientID != "" {
			return doc.ClientID, doc.ClientSecret, nil
		}
		return "", "", errors.New("OAuth credentials file has no client_id (expected a Desktop app JSON)")
	}
	return "", "", errors.New("no Google OAuth client configured: set ONORAB_OAUTH_CLIENT_ID, " +
		"build with -X main.buildOAuthClientID, or set ONORAB_CREDENTIALS_PATH to a Desktop OAuth JSON (dev only)")
}

// buildAuthURL builds the Google sign-in URL for one PKCE session.
// redirectURI is the Hub's loopback listener (any 127.0.0.1 port is
// allowed for Desktop clients); state is echoed back by Google and
// must match, or the callback is rejected.
func buildAuthURL(clientID, redirectURI, state, challenge string) string {
	v := url.Values{}
	v.Set("client_id", clientID)
	v.Set("redirect_uri", redirectURI)
	v.Set("response_type", "code")
	v.Set("scope", driveFileScope)
	v.Set("access_type", "offline") // ask for a refresh token, not just an access token
	v.Set("prompt", "consent")      // force a refresh token even if the user signed in before
	v.Set("code_challenge", challenge)
	v.Set("code_challenge_method", "S256")
	v.Set("state", state)
	return googleAuthURL + "?" + v.Encode()
}

type tokenResponse struct {
	AccessToken  string `json:"access_token"`
	RefreshToken string `json:"refresh_token"`
	ExpiresIn    int64  `json:"expires_in"`
	TokenType    string `json:"token_type"`
	Error        string `json:"error"`
	ErrorDesc    string `json:"error_description"`
}

// DriveAuth owns one PC's Google sign-in: the app client id, the
// user's tokens (refresh token in the OS keychain via TokenStore,
// access token in memory only), and the single in-progress browser
// session. It never hands a token to the plugin; the plugin only ever
// sees "authenticated: true/false" and an auth URL to open.
type DriveAuth struct {
	clientID     string
	clientSecret string
	store        TokenStore
	tokenURL     string

	mu          sync.Mutex
	accessToken string
	expiresAt   time.Time
	// account is the signed-in user's email when known (best effort).
	account string
}

func newDriveAuth(clientID, clientSecret string, store TokenStore) *DriveAuth {
	return &DriveAuth{clientID: clientID, clientSecret: clientSecret, store: store, tokenURL: googleTokenURL}
}

// Authenticated reports whether a usable sign-in exists: a live access
// token, or a stored refresh token that can mint one.
func (a *DriveAuth) Authenticated() bool {
	a.mu.Lock()
	defer a.mu.Unlock()
	if a.accessToken != "" && time.Now().Before(a.expiresAt) {
		return true
	}
	if a.store == nil {
		return false
	}
	_, err := a.store.Load("default")
	return err == nil
}

// Account returns the signed-in email when known, "" otherwise.
func (a *DriveAuth) Account() string {
	a.mu.Lock()
	defer a.mu.Unlock()
	return a.account
}

// GetAccessToken returns a live access token, refreshing from the
// stored refresh token when needed. This is what makes sign-in
// automatic after the first browser login: the Hub restarts, loads
// the refresh token from Credential Manager, and mints a fresh
// access token without opening a browser — until the refresh token
// itself expires (7 days in Google Testing mode; see §3.4) or is
// revoked, at which point the user signs in again via Connect Drive.
func (a *DriveAuth) GetAccessToken() (string, error) {
	a.mu.Lock()
	if a.accessToken != "" && time.Now().Add(60*time.Second).Before(a.expiresAt) {
		tok := a.accessToken
		a.mu.Unlock()
		return tok, nil
	}
	a.mu.Unlock()

	if a.store == nil {
		return "", errors.New("no token store configured")
	}
	refresh, err := a.store.Load("default")
	if err != nil {
		return "", errors.New("Drive is not connected: sign in via Settings → Connect Drive")
	}
	resp, err := a.exchange(map[string]string{
		"grant_type":    "refresh_token",
		"refresh_token": refresh,
	})
	if err != nil {
		return "", err
	}
	if resp.RefreshToken != "" && a.store != nil {
		_ = a.store.Save("default", resp.RefreshToken)
	}
	a.mu.Lock()
	a.accessToken = resp.AccessToken
	a.expiresAt = time.Now().Add(time.Duration(resp.ExpiresIn) * time.Second)
	a.mu.Unlock()
	return resp.AccessToken, nil
}

// exchange posts one token request (code exchange or refresh) and
// decodes the response. Client secret is included only when one is
// configured (dev file); shipped PKCE builds omit it.
func (a *DriveAuth) exchange(form map[string]string) (*tokenResponse, error) {
	v := url.Values{}
	for k, val := range form {
		v.Set(k, val)
	}
	v.Set("client_id", a.clientID)
	if a.clientSecret != "" {
		v.Set("client_secret", a.clientSecret)
	}
	resp, err := http.PostForm(a.tokenURL, v)
	if err != nil {
		return nil, fmt.Errorf("Google token request failed: %w", err)
	}
	defer resp.Body.Close()
	var tr tokenResponse
	if err := json.NewDecoder(resp.Body).Decode(&tr); err != nil {
		return nil, fmt.Errorf("decode Google token response: %w", err)
	}
	if tr.Error != "" {
		return nil, fmt.Errorf("Google token error %s: %s", tr.Error, tr.ErrorDesc)
	}
	if tr.AccessToken == "" {
		return nil, errors.New("Google token response had no access token")
	}
	return &tr, nil
}

// startBrowserSession begins one sign-in: it opens a loopback listener,
// builds the auth URL for the plugin to open in the user's browser,
// and returns the URL. Completion happens asynchronously in
// waitForCallback; the caller (handleAuthStart) returns the URL
// immediately and must call oauth.Finish() exactly once when the
// session ends (success, error, or timeout).
func (a *DriveAuth) startBrowserSession() (authURL string, done chan error, err error) {
	verifier, challenge, err := pkcePair()
	if err != nil {
		return "", nil, err
	}
	stateBytes := make([]byte, 16)
	if _, err := rand.Read(stateBytes); err != nil {
		return "", nil, err
	}
	state := hex.EncodeToString(stateBytes)

	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return "", nil, fmt.Errorf("open loopback listener for Google redirect: %w", err)
	}
	redirectURI := "http://" + ln.Addr().String() + "/"
	authURL = buildAuthURL(a.clientID, redirectURI, state, challenge)

	done = make(chan error, 1)
	go func() {
		defer ln.Close()
		codeCh := make(chan string, 1)
		errCh := make(chan error, 1)
		mux := http.NewServeMux()
		mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
			if r.URL.Query().Get("state") != state {
				http.Error(w, "state mismatch — this sign-in was not started by Onorab Hub", http.StatusBadRequest)
				errCh <- errors.New("Google redirect state mismatch")
				return
			}
			if e := r.URL.Query().Get("error"); e != "" {
				http.Error(w, "Google sign-in was not approved: "+e, http.StatusBadRequest)
				errCh <- fmt.Errorf("Google sign-in error: %s", e)
				return
			}
			code := r.URL.Query().Get("code")
			if code == "" {
				http.Error(w, "no authorization code in Google redirect", http.StatusBadRequest)
				errCh <- errors.New("Google redirect had no authorization code")
				return
			}
			w.Header().Set("Content-Type", "text/html")
			_, _ = w.Write([]byte("<html><body><h2>Onorab is connected.</h2>" +
				"<p>You can close this tab and return to FL Studio.</p></body></html>"))
			codeCh <- code
		})
		go func() { _ = http.Serve(ln, mux) }()
		select {
		case code := <-codeCh:
			tr, err := a.exchange(map[string]string{
				"grant_type":    "authorization_code",
				"code":          code,
				"redirect_uri":  redirectURI,
				"code_verifier": verifier,
			})
			if err != nil {
				done <- err
				return
			}
			if tr.RefreshToken == "" {
				done <- errors.New("Google did not return a refresh token — sign in again and approve access")
				return
			}
			if a.store != nil {
				if err := a.store.Save("default", tr.RefreshToken); err != nil {
					done <- fmt.Errorf("store refresh token in OS keychain: %w", err)
					return
				}
			}
			a.mu.Lock()
			a.accessToken = tr.AccessToken
			a.expiresAt = time.Now().Add(time.Duration(tr.ExpiresIn) * time.Second)
			a.mu.Unlock()
			done <- nil
		case err := <-errCh:
			done <- err
		case <-time.After(5 * time.Minute):
			done <- errors.New("Google sign-in timed out after 5 minutes — press Connect Drive again")
		}
	}()
	return authURL, done, nil
}
