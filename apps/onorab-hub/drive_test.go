package main

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
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
	id, secret, err := resolveOAuthClient("")
	if err != nil || id != "env-client-id" || secret != "" {
		t.Fatalf("env resolution wrong: id=%q secret=%q err=%v", id, secret, err)
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
