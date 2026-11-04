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
	"sync/atomic"
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
//  1. ONORAB_OAUTH_CLIENT_ID environment variable (dev / manual runs),
//     with ONORAB_OAUTH_CLIENT_SECRET when one is configured.
//  2. buildOAuthClientID + buildOAuthClientSecret, injected at
//     release-build time by CI from GitHub Actions secrets (see
//     docs/architecture.md §3.4). The repo itself contains NO client
//     id and NO client secret. The id and the secret must always come
//     from the SAME source: a distributed build that carries only an
//     id (and silently drops the secret) fails exactly like the
//     reported bug — browser opens, Google approves, but the code
//     exchange fails and /auth/status stays authenticated=false.
//     A distribution build (buildDistribution=true) therefore FAILS
//     FAST at startup when its secret is missing, instead of
//     pretending sign-in works.
//  3. A Desktop OAuth JSON file at ONORAB_CREDENTIALS_PATH (dev only,
//     e.g. the file downloaded from Google Cloud Console). This path
//     is for the developer's own testing; shipped builds use (1)/(2).
//
// Why a secret in the distributed binary is acceptable here: for
// Google Desktop (installed) apps, the client secret is NOT treated
// as a confidential secret — Google's model assumes it ships inside
// the app, and the secret ALONE grants nothing: it only works
// together with a code the user approved in their own browser, under
// PKCE + state verification, for drive.file scope only. What must
// truly stay private — each user's refresh token — is never in the
// binary or the repo: it lives only in that PC's OS keychain. No
// backend proxy is needed, because no truly-private secret is
// distributed; if one ever were, THAT part would have to move to a
// backend instead of into the exe.
//
// Scope is drive.file ONLY: the app sees files it created, plus files
// explicitly shared with the signed-in user for Onorab projects —
// never the user's whole Drive.

// buildOAuthClientID / buildOAuthClientSecret are set by the release
// build:
//   go build -ldflags "-X main.buildOAuthClientID=<id> -X main.buildOAuthClientSecret=<secret> -X main.buildDistribution=true"
// They are empty in plain `go build` / `go test` runs.
// buildDistribution is "true" only for the shipped build; a
// distribution build with an id but no secret is a misconfiguration
// and must fail fast (see resolveOAuthClient), never ship silently
// broken.
var buildOAuthClientID string
var buildOAuthClientSecret string
var buildDistribution string

const (
	googleAuthURL     = "https://accounts.google.com/o/oauth2/v2/auth"
	googleTokenURL    = "https://oauth2.googleapis.com/token"
	googleUserinfoURL = "https://openidconnect.googleapis.com/v1/userinfo"
	googleRevokeURL   = "https://oauth2.googleapis.com/revoke"
	driveFileScope    = "https://www.googleapis.com/auth/drive.file"
)

// resolveOAuthClient returns (clientID, clientSecret, error) following
// the precedence above. The id and the secret always come as a pair
// from ONE source: env, baked build values, or the JSON file. Mixing
// sources (e.g. a baked id with a JSON secret) would pair a client
// with another client's secret and fail the exchange with a confusing
// error, so it is never done.
func resolveOAuthClient(credentialsPath string) (string, string, error) {
	if id := strings.TrimSpace(os.Getenv("ONORAB_OAUTH_CLIENT_ID")); id != "" {
		return id, strings.TrimSpace(os.Getenv("ONORAB_OAUTH_CLIENT_SECRET")), nil
	}
	if id := strings.TrimSpace(buildOAuthClientID); id != "" {
		secret := strings.TrimSpace(buildOAuthClientSecret)
		if strings.EqualFold(strings.TrimSpace(buildDistribution), "true") && secret == "" {
			return "", "", errors.New("distribution build is missing its OAuth client secret: " +
				"build with -X main.buildOAuthClientSecret from the ONORAB_OAUTH_CLIENT_SECRET secret " +
				"(and -X main.buildDistribution=true); a distribution build must carry the same " +
				"client id + secret pair as the Desktop OAuth JSON, or the code exchange fails " +
				"and sign-in silently stays unauthenticated")
		}
		return id, secret, nil
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
	// userinfoURL / revokeURL are fields (not constants) so tests can
	// point them at a local stub server. Production defaults are the
	// Google endpoints (see newDriveAuth).
	userinfoURL string
	revokeURL   string

	mu          sync.Mutex
	accessToken string
	expiresAt   time.Time
	// account is the signed-in user's email when known (best effort).
	// It is filled from Google's userinfo endpoint after sign-in or
	// refresh, persisted in the OS keychain under "default:account"
	// (a SEPARATE entry from the refresh token, so existing stored
	// tokens keep working unchanged), and reported by /auth/status so
	// the plugin can show the connected account WITHOUT ever asking
	// the user to type their Gmail again. Empty means "unknown", never
	// "no account": Authenticated() is the source of truth for that.
	account string
	// lastAuthError is the last sign-in failure in plain words (user
	// cancelled, Google refused the exchange, the OS keychain could
	// not store the token, sign-in timed out), or "" when the last
	// attempt succeeded or none ran. /auth/status reports it so the
	// plugin can show a message + "Try again" instead of a silent
	// authenticated=false. It never contains a token, code, or
	// secret: only Google's error code/description and Hub-side
	// messages. Cleared when a new attempt starts and when one
	// succeeds; Disconnect clears it too.
	lastAuthError string
}

func newDriveAuth(clientID, clientSecret string, store TokenStore) *DriveAuth {
	return &DriveAuth{clientID: clientID, clientSecret: clientSecret, store: store,
		tokenURL: googleTokenURL, userinfoURL: googleUserinfoURL, revokeURL: googleRevokeURL}
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

// LastAuthError returns the last sign-in failure message, "" if none.
func (a *DriveAuth) LastAuthError() string {
	a.mu.Lock()
	defer a.mu.Unlock()
	return a.lastAuthError
}

func (a *DriveAuth) setLastAuthError(msg string) {
	a.mu.Lock()
	a.lastAuthError = msg
	a.mu.Unlock()
}

func (a *DriveAuth) clearLastAuthError() {
	a.mu.Lock()
	a.lastAuthError = ""
	a.mu.Unlock()
}

// fetchAccount asks Google who the access token belongs to. It is
// best-effort by design: a failed lookup must never break sign-in or
// token refresh — the account is display-only, never a gate.
func (a *DriveAuth) fetchAccount(accessToken string) (string, error) {
	req, err := http.NewRequest(http.MethodGet, a.userinfoURL, nil)
	if err != nil {
		return "", err
	}
	req.Header.Set("Authorization", "Bearer "+accessToken)
	client := &http.Client{Timeout: 10 * time.Second}
	resp, err := client.Do(req)
	if err != nil {
		return "", err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return "", fmt.Errorf("userinfo request returned %s", resp.Status)
	}
	var doc struct {
		Email string `json:"email"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&doc); err != nil {
		return "", err
	}
	return strings.TrimSpace(doc.Email), nil
}

// tryFetchAccount fills the account once, from a live access token,
// and persists it next to the token. It never holds the mutex during
// the HTTP call, and it never fails the caller: sign-in works fine
// with an unknown account; only the Settings display loses the email.
func (a *DriveAuth) tryFetchAccount(accessToken string) {
	a.mu.Lock()
	if a.account != "" {
		a.mu.Unlock()
		return
	}
	a.mu.Unlock()
	email, err := a.fetchAccount(accessToken)
	if err != nil || email == "" {
		return
	}
	a.mu.Lock()
	a.account = email
	a.mu.Unlock()
	if a.store != nil {
		_ = a.store.Save("default:account", email)
	}
}

// EnsureAccount restores the account display after a Hub restart:
// first from the persisted "default:account" entry (covers tokens
// stored by older builds that never saved an email), otherwise from
// a fresh userinfo lookup with a minted access token. Best-effort:
// it silently does nothing when no sign-in exists.
func (a *DriveAuth) EnsureAccount() {
	if a.store != nil {
		if email, err := a.store.Load("default:account"); err == nil && email != "" {
			a.mu.Lock()
			a.account = email
			a.mu.Unlock()
			return
		}
	}
	if tok, err := a.GetAccessToken(); err == nil {
		a.tryFetchAccount(tok)
	}
}

// Disconnect signs the user out on this PC: it revokes the refresh
// token at Google (best-effort — a network failure must not block
// local sign-out), then deletes the token AND the stored account from
// the OS keychain and clears the in-memory session. After this the
// plugin shows "Not connected" again and a fresh Connect starts a
// clean browser sign-in.
func (a *DriveAuth) Disconnect() {
	var refresh string
	if a.store != nil {
		refresh, _ = a.store.Load("default")
	}
	if refresh != "" {
		v := url.Values{}
		v.Set("token", refresh)
		if req, err := http.NewRequest(http.MethodPost, a.revokeURL, strings.NewReader(v.Encode())); err == nil {
			req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
			client := &http.Client{Timeout: 10 * time.Second}
			if resp, err := client.Do(req); err == nil {
				_ = resp.Body.Close()
			}
		}
	}
	a.mu.Lock()
	a.accessToken = ""
	a.expiresAt = time.Time{}
	a.account = ""
	a.mu.Unlock()
	if a.store != nil {
		_ = a.store.Delete("default")
		_ = a.store.Delete("default:account")
	}
	a.clearLastAuthError()
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
		// A revoked/expired refresh token is permanent: keeping it
		// would fail every call with the same error and leave the
		// plugin stuck on "not connected" with no path forward.
		// Delete it (token + stored account) so /auth/status is
		// cleanly unauthenticated and a fresh Connect starts clean.
		// Transient failures (network, Google 5xx) keep the token:
		// only invalid_grant proves the grant itself is dead.
		if strings.Contains(err.Error(), "invalid_grant") {
			a.mu.Lock()
			a.accessToken = ""
			a.expiresAt = time.Time{}
			a.account = ""
			a.mu.Unlock()
			_ = a.store.Delete("default")
			_ = a.store.Delete("default:account")
			return "", errors.New("Drive access was revoked or expired — sign in again via Settings → Connect Drive")
		}
		return "", err
	}
	if resp.RefreshToken != "" && a.store != nil {
		_ = a.store.Save("default", resp.RefreshToken)
	}
	a.mu.Lock()
	a.accessToken = resp.AccessToken
	a.expiresAt = time.Now().Add(time.Duration(resp.ExpiresIn) * time.Second)
	a.mu.Unlock()
	// Fill the account display on the same refresh that proved the
	// token still works (best-effort; a failed lookup changes nothing).
	a.tryFetchAccount(resp.AccessToken)
	return resp.AccessToken, nil
}

// exchange posts one token request (code exchange or refresh) and
// decodes the response. The client secret is included when one is
// configured (distribution build, dev env, or dev JSON); a build with
// an id but no secret sends id + PKCE only. Google's error text is
// returned verbatim — it contains no tokens, codes, or secrets.
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
// and returns the URL. Completion happens asynchronously on the done
// channel; the caller (handleAuthStart) must call oauth.Finish()
// exactly once when the session ends (success, error, or timeout).
//
// The browser success page is written ONLY after the code exchange
// AND the refresh-token store both succeed, inside the redirect
// handler itself. The previous version wrote "Onorab is connected"
// the moment the code arrived and exchanged afterwards, so a failed
// exchange still looked like a success in the browser while
// /auth/status stayed authenticated=false. Every failure path now
// writes an error page, records a plain-words message in
// lastAuthError (reported by /auth/status for the plugin's
// "Try again"), and sends the error on done. No page or message ever
// echoes the code, a token, or the client secret.
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

	// A fresh attempt replaces any old failure report; a failure
	// below sets a new one, success clears it.
	a.clearLastAuthError()
	done = make(chan error, 1)
	go func() {
		defer ln.Close()
		resultCh := make(chan error, 1)
		// Exactly one redirect is processed per session: a second hit
		// (browser refresh of the success page, double redirect) must
		// never re-exchange an already-used code and overwrite the
		// session result with a code-reuse error.
		var handled atomic.Bool
		mux := http.NewServeMux()
		mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
			if !handled.CompareAndSwap(false, true) {
				w.Header().Set("Content-Type", "text/html")
				_, _ = w.Write([]byte("<html><body><h2>Onorab sign-in already finished.</h2>" +
					"<p>Check FL Studio for the result.</p></body></html>"))
				return
			}
			fail := func(status int, msg string, err error) {
				http.Error(w, msg+". Close this tab and press Try again in FL Studio.", status)
				a.setLastAuthError(msg)
				resultCh <- err
			}
			if r.URL.Query().Get("state") != state {
				msg := "Google redirect state mismatch — this sign-in was not started by Onorab Hub"
				fail(http.StatusBadRequest, msg, errors.New(msg))
				return
			}
			if e := r.URL.Query().Get("error"); e != "" {
				// access_denied = the user pressed Cancel (or denied
				// access) on Google's page. Any other error is shown
				// as-is: Google's error codes contain no secrets.
				msg := "Google sign-in error: " + e
				if e == "access_denied" {
					msg = "Google sign-in was cancelled"
				}
				fail(http.StatusBadRequest, msg, errors.New(msg))
				return
			}
			code := r.URL.Query().Get("code")
			if code == "" {
				msg := "Google redirect had no authorization code"
				fail(http.StatusBadRequest, msg, errors.New(msg))
				return
			}
			// Exchange + store FIRST, success page ONLY after both
			// succeed. The code itself is never echoed to the page
			// or the logs.
			tr, err := a.exchange(map[string]string{
				"grant_type":    "authorization_code",
				"code":          code,
				"redirect_uri":  redirectURI,
				"code_verifier": verifier,
			})
			if err != nil {
				fail(http.StatusBadGateway, "Google sign-in could not be completed: "+err.Error(), err)
				return
			}
			if tr.RefreshToken == "" {
				msg := "Google did not return a refresh token — sign in again and approve access"
				fail(http.StatusBadGateway, msg, errors.New(msg))
				return
			}
			if a.store != nil {
				if err := a.store.Save("default", tr.RefreshToken); err != nil {
					msg := "could not save the sign-in on this PC: " + err.Error()
					fail(http.StatusInternalServerError, msg, errors.New(msg))
					return
				}
			}
			a.mu.Lock()
			a.accessToken = tr.AccessToken
			a.expiresAt = time.Now().Add(time.Duration(tr.ExpiresIn) * time.Second)
			a.mu.Unlock()
			a.clearLastAuthError()
			w.Header().Set("Content-Type", "text/html")
			_, _ = w.Write([]byte("<html><body><h2>Onorab is connected.</h2>" +
				"<p>You can close this tab and return to FL Studio.</p></body></html>"))
			// The account display fills in the background: a slow
			// userinfo lookup must not delay the success page or
			// the session result. The plugin polls /auth/status and
			// shows the email as soon as it arrives.
			go a.tryFetchAccount(tr.AccessToken)
			resultCh <- nil
		})
		go func() { _ = http.Serve(ln, mux) }()
		select {
		case err := <-resultCh:
			done <- err
		case <-time.After(5 * time.Minute):
			msg := "Google sign-in timed out after 5 minutes — press Connect Drive again"
			a.setLastAuthError(msg)
			done <- errors.New(msg)
		}
	}()
	return authURL, done, nil
}
