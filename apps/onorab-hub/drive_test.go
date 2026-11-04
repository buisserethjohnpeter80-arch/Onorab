package main

import (
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"testing"
	"time"
)

// RFC 7636 Appendix B test vector: this verifier must produce this
// challenge, or the PKCE flow is broken and Google will reject the
// code exchange.
func TestPKCEVector(t *testing.T) {
	verifier := "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk"
	challenge := pkceChallenge(verifier)
	if challenge != "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM" {
		t.Fatalf("PKCE challenge mismatch: got %q", challenge)
	}
}

func TestAuthURLParams(t *testing.T) {
	u := buildAuthURL("client123", "http://127.0.0.1:54321/", "state9", "challenge9")
	if !strings.Contains(u, "accounts.google.com/o/oauth2/v2/auth") {
		t.Fatalf("auth URL host wrong: %s", u)
	}
	for _, want := range []string{
		"client_id=client123", "response_type=code", "access_type=offline",
		"prompt=consent", "code_challenge_method=S256", "code_challenge=challenge9",
		"state=state9", "redirect_uri=http%3A%2F%2F127.0.0.1%3A54321%2F",
	} {
		if !strings.Contains(u, want) {
			t.Fatalf("auth URL missing %q in %s", want, u)
		}
	}
	if !strings.Contains(u, "scope=https%3A%2F%2Fwww.googleapis.com%2Fauth%2Fdrive.file") {
		t.Fatalf("auth URL scope must be drive.file only: %s", u)
	}
}

func TestResolveClientIDFromEnv(t *testing.T) {
	t.Setenv("ONORAB_OAUTH_CLIENT_ID", "env-client-id")
	t.Setenv("ONORAB_OAUTH_CLIENT_SECRET", "")
	id, secret, err := resolveOAuthClient("")
	if err != nil || id != "env-client-id" || secret != "" {
		t.Fatalf("env resolution wrong: id=%q secret=%q err=%v", id, secret, err)
	}
}

// The distributed build must carry the id + secret as a PAIR (the
// reported bug: an id-only distributed build opened the browser,
// Google approved, but the code exchange failed and /auth/status
// stayed authenticated=false). A distribution build with an id but
// no secret must fail fast instead of shipping silently broken.
func TestResolveOAuthClientBakedPairAndDistributionGuard(t *testing.T) {
	t.Setenv("ONORAB_OAUTH_CLIENT_ID", "")
	t.Setenv("ONORAB_OAUTH_CLIENT_SECRET", "")
	oldID, oldSecret, oldDist := buildOAuthClientID, buildOAuthClientSecret, buildDistribution
	t.Cleanup(func() {
		buildOAuthClientID, buildOAuthClientSecret, buildDistribution = oldID, oldSecret, oldDist
	})

	buildOAuthClientID, buildOAuthClientSecret, buildDistribution = "baked-id", "baked-secret", "true"
	if id, secret, err := resolveOAuthClient(""); err != nil || id != "baked-id" || secret != "baked-secret" {
		t.Fatalf("baked pair wrong: id=%q secret-set=%v err=%v", id, secret != "", err)
	}

	buildOAuthClientSecret = ""
	if _, _, err := resolveOAuthClient(""); err == nil || !strings.Contains(err.Error(), "client secret") {
		t.Fatalf("distribution build without a secret must fail fast, got err=%v", err)
	}

	// A non-distribution (dev) build with an id but no secret is
	// still allowed: PKCE-only, same as before.
	buildDistribution = ""
	if id, secret, err := resolveOAuthClient(""); err != nil || id != "baked-id" || secret != "" {
		t.Fatalf("dev id-only build wrong: id=%q secret=%q err=%v", id, secret, err)
	}

	// Env always wins over baked values, as a pair.
	t.Setenv("ONORAB_OAUTH_CLIENT_ID", "env-id")
	t.Setenv("ONORAB_OAUTH_CLIENT_SECRET", "env-secret")
	if id, secret, err := resolveOAuthClient(""); err != nil || id != "env-id" || secret != "env-secret" {
		t.Fatalf("env pair must win over baked: id=%q secret=%q err=%v", id, secret, err)
	}
}

// startBrowserSession against stub Google endpoints: the test drives
// the loopback redirect itself (parsing state + redirect_uri out of
// the returned auth URL), the way a browser would after Google
// approval.
func TestBrowserSessionSuccessPageOnlyAfterExchange(t *testing.T) {
	tokenSrv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if err := r.ParseForm(); err != nil {
			t.Errorf("token stub form parse: %v", err)
		}
		if r.Form.Get("client_id") != "id" || r.Form.Get("client_secret") != "sec" ||
			r.Form.Get("code") != "testcode" || r.Form.Get("code_verifier") == "" ||
			r.Form.Get("redirect_uri") == "" {
			t.Errorf("token stub got wrong form: %v", r.Form)
		}
		_ = json.NewEncoder(w).Encode(map[string]any{
			"access_token": "access-1", "refresh_token": "refresh-1", "expires_in": 3600,
		})
	}))
	defer tokenSrv.Close()
	userinfoSrv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_ = json.NewEncoder(w).Encode(map[string]any{"email": "singer@example.com"})
	}))
	defer userinfoSrv.Close()

	store := NewMemoryTokenStore()
	auth := newDriveAuth("id", "sec", store)
	auth.tokenURL = tokenSrv.URL
	auth.userinfoURL = userinfoSrv.URL

	authURL, done, err := auth.startBrowserSession()
	if err != nil {
		t.Fatalf("startBrowserSession failed: %v", err)
	}
	u, err := url.Parse(authURL)
	if err != nil {
		t.Fatalf("auth URL unparsable: %v", err)
	}
	redirectURI, state := u.Query().Get("redirect_uri"), u.Query().Get("state")
	resp, err := http.Get(redirectURI + "?code=testcode&state=" + state)
	if err != nil {
		t.Fatalf("redirect GET failed: %v", err)
	}
	defer resp.Body.Close()
	var body strings.Builder
	if _, err := io.Copy(&body, resp.Body); err != nil {
		t.Fatalf("read redirect body: %v", err)
	}
	if !strings.Contains(body.String(), "Onorab is connected") {
		t.Fatalf("success page missing after successful exchange: %q", body.String())
	}
	select {
	case err := <-done:
		if err != nil {
			t.Fatalf("session done must be nil, got %v", err)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("session done timed out")
	}
	if tok, err := store.Load("default"); err != nil || tok != "refresh-1" {
		t.Fatalf("refresh token not stored: tok=%q err=%v", tok, err)
	}
	if auth.LastAuthError() != "" {
		t.Fatalf("lastAuthError must be empty after success, got %q", auth.LastAuthError())
	}
}

func TestBrowserSessionExchangeFailureShowsErrorNotSuccess(t *testing.T) {
	tokenSrv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusBadRequest)
		_ = json.NewEncoder(w).Encode(map[string]any{
			"error": "invalid_client", "error_description": "client not found",
		})
	}))
	defer tokenSrv.Close()

	store := NewMemoryTokenStore()
	auth := newDriveAuth("id", "sec", store)
	auth.tokenURL = tokenSrv.URL

	authURL, done, err := auth.startBrowserSession()
	if err != nil {
		t.Fatalf("startBrowserSession failed: %v", err)
	}
	u, _ := url.Parse(authURL)
	resp, err := http.Get(u.Query().Get("redirect_uri") + "?code=testcode&state=" + u.Query().Get("state"))
	if err != nil {
		t.Fatalf("redirect GET failed: %v", err)
	}
	defer resp.Body.Close()
	var body strings.Builder
	_, _ = io.Copy(&body, resp.Body)
	if strings.Contains(body.String(), "Onorab is connected") {
		t.Fatalf("error page must never say connected: %q", body.String())
	}
	if !strings.Contains(body.String(), "invalid_client") {
		t.Fatalf("error page must name the failure: %q", body.String())
	}
	select {
	case err := <-done:
		if err == nil {
			t.Fatal("session done must carry the exchange error")
		}
	case <-time.After(5 * time.Second):
		t.Fatal("session done timed out")
	}
	if _, err := store.Load("default"); err == nil {
		t.Fatal("no token may be stored after a failed exchange")
	}
	if auth.LastAuthError() == "" {
		t.Fatal("lastAuthError must report the failed exchange")
	}
}

func TestBrowserSessionCancelReportsFailure(t *testing.T) {
	store := NewMemoryTokenStore()
	auth := newDriveAuth("id", "sec", store)

	authURL, done, err := auth.startBrowserSession()
	if err != nil {
		t.Fatalf("startBrowserSession failed: %v", err)
	}
	u, _ := url.Parse(authURL)
	resp, err := http.Get(u.Query().Get("redirect_uri") + "?error=access_denied&state=" + u.Query().Get("state"))
	if err != nil {
		t.Fatalf("redirect GET failed: %v", err)
	}
	defer resp.Body.Close()
	var body strings.Builder
	_, _ = io.Copy(&body, resp.Body)
	if !strings.Contains(body.String(), "cancelled") {
		t.Fatalf("cancel page must say cancelled: %q", body.String())
	}
	select {
	case err := <-done:
		if err == nil {
			t.Fatal("cancelled session must report an error")
		}
	case <-time.After(5 * time.Second):
		t.Fatal("session done timed out")
	}
	if !strings.Contains(auth.LastAuthError(), "cancelled") {
		t.Fatalf("lastAuthError must report the cancel, got %q", auth.LastAuthError())
	}
}

func TestRefreshInvalidGrantClearsStoredToken(t *testing.T) {
	tokenSrv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusBadRequest)
		_ = json.NewEncoder(w).Encode(map[string]any{
			"error": "invalid_grant", "error_description": "Token has been expired or revoked.",
		})
	}))
	defer tokenSrv.Close()

	store := NewMemoryTokenStore()
	_ = store.Save("default", "dead-refresh")
	_ = store.Save("default:account", "singer@example.com")
	auth := newDriveAuth("id", "sec", store)
	auth.tokenURL = tokenSrv.URL

	if _, err := auth.GetAccessToken(); err == nil || !strings.Contains(err.Error(), "revoked") {
		t.Fatalf("revoked refresh must report re-sign-in, got err=%v", err)
	}
	if _, err := store.Load("default"); err == nil {
		t.Fatal("revoked refresh token must be deleted")
	}
	if _, err := store.Load("default:account"); err == nil {
		t.Fatal("stored account must be deleted with a revoked token")
	}
}

// Minimal fake Drive API: folders, files, upload, download, delete,
// permissions. Enough to prove the client maps the folder model and
// the CRUD calls correctly; real Google behaviour is verified by the
// manual Drive test in docs/testing.md.
func fakeDriveServer(t *testing.T) *httptest.Server {
	t.Helper()
	var calls []string
	mux := http.NewServeMux()
	mux.HandleFunc("/drive/v3/files", func(w http.ResponseWriter, r *http.Request) {
		calls = append(calls, r.Method+" /drive/v3/files q="+r.URL.Query().Get("q"))
		if r.Method == http.MethodGet {
			q := r.URL.Query().Get("q")
			// Folder queries use "mimeType = '<folder>'"; file queries
			// use "mimeType != '<folder>'" — the "!=" form must NOT
			// match the folder branch (substring trap).
			if strings.Contains(q, "mimeType = '"+folderMimeType+"'") {
				if strings.Contains(q, "Onorab_projet") && !strings.Contains(q, "in parents") {
					_ = json.NewEncoder(w).Encode(map[string]any{"files": []driveFile{{ID: "root1", Name: "Onorab_projet", MimeType: folderMimeType}}})
					return
				}
				_ = json.NewEncoder(w).Encode(map[string]any{"files": []driveFile{{ID: "proj1", Name: "marcus music", MimeType: folderMimeType, CreatedTime: "2026-11-03T10:00:00Z"}}})
				return
			}
			_ = json.NewEncoder(w).Encode(map[string]any{"files": []driveFile{{ID: "f1", Name: "beat.wav", MimeType: "audio/wav", Size: "1234", CreatedTime: "2026-11-03T11:00:00Z"}}})
			return
		}
		if r.Method == http.MethodPost {
			_ = json.NewEncoder(w).Encode(driveFile{ID: "newfolder", Name: "x", MimeType: folderMimeType})
			return
		}
	})
	mux.HandleFunc("/drive/v3/files/", func(w http.ResponseWriter, r *http.Request) {
		calls = append(calls, r.Method+" "+r.URL.Path)
		if strings.HasSuffix(r.URL.Path, "/permissions") {
			_ = json.NewEncoder(w).Encode(map[string]any{"id": "perm1"})
			return
		}
		if r.Method == http.MethodGet && r.URL.Query().Get("alt") == "media" {
			_, _ = w.Write([]byte("WAVBYTES"))
			return
		}
		if r.Method == http.MethodGet {
			_ = json.NewEncoder(w).Encode(driveFile{ID: "f1", Name: "beat.wav", MimeType: "audio/wav", Size: "1234", CreatedTime: "2026-11-03T11:00:00Z"})
			return
		}
		if r.Method == http.MethodDelete {
			w.WriteHeader(http.StatusNoContent)
			return
		}
	})
	mux.HandleFunc("/upload/drive/v3/files", func(w http.ResponseWriter, r *http.Request) {
		calls = append(calls, "POST upload")
		_ = json.NewEncoder(w).Encode(driveFile{ID: "up1", Name: "new.wav", MimeType: "audio/wav", Size: "99", CreatedTime: "2026-11-03T12:00:00Z"})
	})
	_ = calls
	return httptest.NewServer(mux)
}

func TestDriveClientListUploadDownloadDeleteShare(t *testing.T) {
	srv := fakeDriveServer(t)
	defer srv.Close()
	c := newDriveClient(func() (string, error) { return "test-token", nil })
	c.apiBase = srv.URL + "/drive/v3"
	c.uploadBase = srv.URL + "/upload/drive/v3"

	files, err := c.ListAll()
	if err != nil {
		t.Fatalf("ListAll: %v", err)
	}
	if len(files) == 0 {
		t.Fatalf("ListAll returned no files")
	}
	// Creation order: oldest first (project file 11:00 before upload
	// time is irrelevant here; ListAll sorts by createdTime).
	for i := 1; i < len(files); i++ {
		if files[i].ModifiedTime < files[i-1].ModifiedTime {
			t.Fatalf("ListAll not in creation order: %v", files)
		}
	}

	up, err := c.Upload("marcus music", "new.wav", []byte("data"), "audio/wav")
	if err != nil {
		t.Fatalf("Upload: %v", err)
	}
	if up.ID != "up1" || up.ProjectID == nil || *up.ProjectID != "marcus music" {
		t.Fatalf("Upload mapped wrong: %+v", up)
	}

	data, err := c.Download("f1")
	if err != nil || string(data) != "WAVBYTES" {
		t.Fatalf("Download: %q %v", data, err)
	}

	if err := c.Delete("f1"); err != nil {
		t.Fatalf("Delete: %v", err)
	}

	perm, err := c.Share("marcus music", "friend@gmail.com")
	if err != nil || perm != "perm1" {
		t.Fatalf("Share: %q %v", perm, err)
	}
}

// Account display + disconnect (2026-11-04): the plugin must show the
// connected Gmail without asking the user to type it, and Disconnect
// must clear both the token and the stored account.
func TestFetchAccountFromUserinfo(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Authorization") != "Bearer tok123" {
			http.Error(w, "unauthorized", http.StatusUnauthorized)
			return
		}
		_ = json.NewEncoder(w).Encode(map[string]any{"email": "singer@example.com"})
	}))
	defer srv.Close()
	auth := newDriveAuth("id", "", NewMemoryTokenStore())
	auth.userinfoURL = srv.URL
	email, err := auth.fetchAccount("tok123")
	if err != nil || email != "singer@example.com" {
		t.Fatalf("fetchAccount wrong: email=%q err=%v", email, err)
	}
}

func TestDisconnectClearsTokenAndAccount(t *testing.T) {
	revoked := make(chan struct{}, 1)
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		select {
		case revoked <- struct{}{}:
		default:
		}
		w.WriteHeader(http.StatusOK)
	}))
	defer srv.Close()
	store := NewMemoryTokenStore()
	_ = store.Save("default", "refresh-abc")
	_ = store.Save("default:account", "singer@example.com")
	auth := newDriveAuth("id", "", store)
	auth.revokeURL = srv.URL
	auth.mu.Lock()
	auth.account = "singer@example.com"
	auth.accessToken = "tok123"
	auth.mu.Unlock()
	auth.Disconnect()
	if _, err := store.Load("default"); err == nil {
		t.Fatalf("Disconnect left the refresh token stored")
	}
	if _, err := store.Load("default:account"); err == nil {
		t.Fatalf("Disconnect left the account stored")
	}
	if auth.Account() != "" {
		t.Fatalf("Disconnect left account %q in memory", auth.Account())
	}
	select {
	case <-revoked:
	default:
		t.Fatalf("Disconnect did not attempt the Google revoke call")
	}
}

func TestEnsureAccountLoadsStoredEmailWithoutTokenRefresh(t *testing.T) {
	store := NewMemoryTokenStore()
	_ = store.Save("default:account", "stored@example.com")
	// No "default" token is stored on purpose: EnsureAccount must
	// return the persisted email WITHOUT touching the token endpoint.
	auth := newDriveAuth("id", "", store)
	auth.tokenURL = "http://127.0.0.1:1/invalid" // would fail if called
	auth.EnsureAccount()
	if auth.Account() != "stored@example.com" {
		t.Fatalf("EnsureAccount wrong: account=%q", auth.Account())
	}
}

func TestUploadUpdatesExistingFileInsteadOfDuplicating(t *testing.T) {
	run := func(existingName string) (created bool, updatedID, updatedBody string, up OnorabFile, err error) {
		srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			switch {
			case r.Method == http.MethodGet && strings.HasPrefix(r.URL.Path, "/drive/v3/files"):
				q := r.URL.Query().Get("q")
				var files []driveFile
				if strings.Contains(q, "mimeType = '") {
					// Folder search (root or project folder).
					if strings.Contains(q, "Onorab_projet") {
						files = []driveFile{{ID: "root1", Name: "Onorab_projet", MimeType: folderMimeType}}
					} else if strings.Contains(q, "marcus music") {
						files = []driveFile{{ID: "proj1", Name: "marcus music", MimeType: folderMimeType}}
					}
				} else if existingName != "" {
					// File search: the stub lists whatever name the
					// scenario sets, so the client-side name filter
					// is what decides create vs update.
					files = []driveFile{{ID: "f-exist", Name: existingName, MimeType: "audio/wav", Size: "4", CreatedTime: "2026-11-04T00:00:00Z"}}
				}
				_ = json.NewEncoder(w).Encode(map[string]any{"files": files})
			case r.Method == http.MethodPost && strings.HasPrefix(r.URL.Path, "/upload/drive/v3/files"):
				created = true
				_ = json.NewEncoder(w).Encode(driveFile{ID: "up-new", Name: "Kick_push-1.wav", MimeType: "audio/wav", Size: "4", CreatedTime: "2026-11-04T00:00:00Z"})
			case r.Method == http.MethodPatch && strings.HasPrefix(r.URL.Path, "/upload/drive/v3/files/"):
				updatedID = strings.TrimPrefix(r.URL.Path, "/upload/drive/v3/files/")
				body, _ := io.ReadAll(r.Body)
				updatedBody = string(body)
				_ = json.NewEncoder(w).Encode(driveFile{ID: updatedID, Name: "Kick_push-1.wav", MimeType: "audio/wav", Size: "4", CreatedTime: "2026-11-04T00:00:00Z"})
			default:
				t.Errorf("unexpected request: %s %s", r.Method, r.URL.Path)
				w.WriteHeader(http.StatusNotFound)
			}
		}))
		defer srv.Close()
		c := newDriveClient(func() (string, error) { return "test-token", nil })
		c.apiBase = srv.URL + "/drive/v3"
		c.uploadBase = srv.URL + "/upload/drive/v3"
		up, err = c.Upload("marcus music", "Kick_push-1.wav", []byte("WAV!"), "audio/wav")
		return created, updatedID, updatedBody, up, err
	}

	// No existing file: a plain create, no update.
	created, updatedID, updatedBody, up, err := run("")
	if err != nil {
		t.Fatalf("Upload (no existing): %v", err)
	}
	if !created || updatedID != "" || up.ID != "up-new" {
		t.Fatalf("Upload (no existing) must create: created=%v updated=%q id=%q", created, updatedID, up.ID)
	}

	// Same name already in the folder: update it, never create.
	created, updatedID, updatedBody, up, err = run("Kick_push-1.wav")
	if err != nil {
		t.Fatalf("Upload (existing): %v", err)
	}
	if created || updatedID != "f-exist" || up.ID != "f-exist" {
		t.Fatalf("Upload (existing) must update f-exist: created=%v updated=%q id=%q", created, updatedID, up.ID)
	}
	if updatedBody != "WAV!" {
		t.Fatalf("update body must carry the new bytes, got %q", updatedBody)
	}

	// A different name listed: the client-side filter must not mistake
	// it for the target, so this is still a create.
	created, updatedID, _, up, err = run("Bass_push-1.wav")
	if err != nil {
		t.Fatalf("Upload (different name): %v", err)
	}
	if !created || updatedID != "" || up.ID != "up-new" {
		t.Fatalf("Upload (different name) must create: created=%v updated=%q id=%q", created, updatedID, up.ID)
	}
}
