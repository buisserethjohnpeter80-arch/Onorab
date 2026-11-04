package main

import (
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"
)

// OnorabFile mirrors packages/protocol/file.json. Field names are the
// contract shared with the VST3 plugin and the mobile app.
type OnorabFile struct {
	ID           string  `json:"id"`
	Name         string  `json:"name"`
	MimeType     string  `json:"mimeType"`
	SizeBytes    int64   `json:"sizeBytes"`
	ModifiedTime string  `json:"modifiedTime"`
	ProjectID    *string `json:"projectId"`
	Source       string  `json:"source"`
}

// OnorabInvite mirrors packages/protocol/invite.json. Phase 2 status:
// in the Mock provider an invite is a local record only (Hub restarts
// forget it). The real Drive provider turns an invite into a Drive
// sharing permission (permissions.create) for that Gmail on the folder
// Onorab_projet/<project>/ — the invited user then sees the same
// project, in the same creation order, from their own FL Studio.
type OnorabInvite struct {
	Email     string `json:"email"`
	Project   string `json:"project"`
	Status    string `json:"status"` // pending | accepted (Phase 2)
	CreatedAt string `json:"createdAt"`
}

// OnorabTransfer mirrors packages/protocol/transfer.json.
// Status: queued | uploading | downloading | completed | failed.
// Progress is 0-100.
type OnorabTransfer struct {
	ID        string  `json:"id"`
	FileID    string  `json:"fileId"`
	FileName  string  `json:"fileName"`
	Direction string  `json:"direction"` // upload | download
	Status    string  `json:"status"`
	Progress  float64 `json:"progress"`
	Error     *string `json:"error"`
}

// OnorabEvent mirrors packages/protocol/event.json.
type OnorabEvent struct {
	Type      string          `json:"type"`
	Timestamp string          `json:"timestamp"`
	Transfer  *OnorabTransfer `json:"transfer,omitempty"`
	File      *OnorabFile     `json:"file,omitempty"`
	Message   string          `json:"message,omitempty"`
}

// ErrorBody mirrors packages/protocol/error.json.
type ErrorBody struct {
	Error struct {
		Code    string `json:"code"`
		Message string `json:"message"`
	} `json:"error"`
}

func writeError(w http.ResponseWriter, status int, code, format string, args ...any) {
	body := ErrorBody{}
	body.Error.Code = code
	body.Error.Message = fmt.Sprintf(format, args...)
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(body)
	log.Printf("onorab-hub: %s: %s", code, body.Error.Message)
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}

// transition validates the transfer state machine. Invalid transitions
// return an error instead of silently changing state.
func (t *OnorabTransfer) transition(to string) error {
	allowed := map[string][]string{
		"queued":      {"uploading", "downloading", "failed"},
		"uploading":   {"completed", "failed"},
		"downloading": {"completed", "failed"},
		"completed":   {},
		"failed":      {},
	}
	for _, next := range allowed[t.Status] {
		if next == to {
			t.Status = to
			return nil
		}
	}
	return fmt.Errorf("invalid transfer transition %s -> %s", t.Status, to)
}

// Store is the Hub's in-memory index plus its on-disk cache. The cache
// holds audio bytes only — never tokens, credentials, or project state.
// fileOrder is the creation order of files (seed order first, then
// uploads in upload order). listFiles returns files in fileOrder,
// NEVER sorted alphabetically: the Cloud Library and Projects lists
// must show sounds in the order the project was created, so an invited
// user's upload appears at the end, in order, not re-sorted.
type Store struct {
	mu        sync.Mutex
	files     map[string]OnorabFile
	fileOrder []string // file insertion order (creation order)
	transfers map[string]*OnorabTransfer
	order     []string // transfer insertion order
	invites   []OnorabInvite
	cacheDir  string
	events    *EventHub
}

func NewStore(cacheDir string) (*Store, error) {
	if err := os.MkdirAll(cacheDir, 0o700); err != nil {
		return nil, fmt.Errorf("create cache dir: %w", err)
	}
	s := &Store{
		files:     map[string]OnorabFile{},
		transfers: map[string]*OnorabTransfer{},
		cacheDir:  cacheDir,
		events:    NewEventHub(),
	}
	s.seedMockFiles()
	return s, nil
}

func newID(prefix string) string {
	b := make([]byte, 8)
	if _, err := rand.Read(b); err != nil {
		// rand.Read failing is fatal for ID uniqueness; do not silently
		// fall back to a timestamp-only ID.
		panic(fmt.Sprintf("onorab-hub: crypto/rand failed: %v", err))
	}
	return prefix + "_" + hex.EncodeToString(b)
}

func nowISO() string { return time.Now().UTC().Format(time.RFC3339) }

// seedMockFiles creates the Phase 1 Cloud Library. These are metadata
// records; their download bytes are generated on demand (see mock.go),
// so the library is populated without any Google account.
func (s *Store) seedMockFiles() {
	now := nowISO()
	for _, f := range []OnorabFile{
		{ID: "mock_voice_memo", Name: "voice-memo-from-phone.wav", MimeType: "audio/wav", SizeBytes: int64(len(generateToneWAV(440, 1))), ModifiedTime: now, ProjectID: nil, Source: "whatsapp"},
		{ID: "mock_beat_idea", Name: "beat-idea-01.wav", MimeType: "audio/wav", SizeBytes: int64(len(generateToneWAV(220, 2))), ModifiedTime: now, ProjectID: nil, Source: "files"},
		{ID: "mock_fl_capture", Name: "fl-capture-demo.wav", MimeType: "audio/wav", SizeBytes: int64(len(generateToneWAV(330, 1))), ModifiedTime: now, ProjectID: nil, Source: "hub"},
	} {
		s.files[f.ID] = f
		s.fileOrder = append(s.fileOrder, f.ID)
	}
}

func (s *Store) listFiles() []OnorabFile {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := make([]OnorabFile, 0, len(s.fileOrder))
	for _, id := range s.fileOrder {
		if f, ok := s.files[id]; ok {
			out = append(out, f)
		}
	}
	return out
}

// deleteFile removes a file from the index and its cached bytes (for
// uploaded files; mock seeded files have no cached bytes, so only the
// index entry goes — and a Hub restart re-seeds them, which is a Mock
// limitation, not a Drive behaviour). It returns the deleted file.
func (s *Store) deleteFile(id string) (OnorabFile, bool) {
	s.mu.Lock()
	file, ok := s.files[id]
	if !ok {
		s.mu.Unlock()
		return OnorabFile{}, false
	}
	delete(s.files, id)
	kept := s.fileOrder[:0]
	for _, other := range s.fileOrder {
		if other != id {
			kept = append(kept, other)
		}
	}
	s.fileOrder = kept
	cacheDir := s.cacheDir
	events := s.events
	s.mu.Unlock()

	// Cached bytes for uploads are stored as <id>_<name> (see
	// handleUpload); remove every cache entry with that prefix.
	if entries, err := os.ReadDir(cacheDir); err == nil {
		for _, e := range entries {
			if strings.HasPrefix(e.Name(), id+"_") {
				_ = os.Remove(filepath.Join(cacheDir, e.Name()))
			}
		}
	}
	events.Broadcast(OnorabEvent{Type: "file.deleted", Timestamp: nowISO(), File: &file})
	return file, true
}

func (s *Store) listTransfers() []OnorabTransfer {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := make([]OnorabTransfer, 0, len(s.transfers))
	for _, id := range s.order {
		if t, ok := s.transfers[id]; ok {
			out = append(out, *t)
		}
	}
	return out
}

// recordTransfer appends a transfer and broadcasts its started event.
func (s *Store) recordTransfer(t *OnorabTransfer) {
	s.mu.Lock()
	s.transfers[t.ID] = t
	s.order = append(s.order, t.ID)
	s.mu.Unlock()
	s.events.Broadcast(OnorabEvent{Type: "transfer.started", Timestamp: nowISO(), Transfer: t})
}

func (s *Store) finishTransfer(t *OnorabTransfer, err error) {
	s.mu.Lock()
	if err != nil {
		msg := err.Error()
		t.Error = &msg
		_ = t.transition("failed")
	} else {
		t.Progress = 100
		_ = t.transition("completed")
	}
	done := *t
	s.mu.Unlock()
	typ := "transfer.completed"
	if err != nil {
		typ = "transfer.failed"
	}
	s.events.Broadcast(OnorabEvent{Type: typ, Timestamp: nowISO(), Transfer: &done})
}

// Server wires HTTP routes to the store.
type Server struct {
	cfg   Config
	store *Store
	oauth *OAuthGuard
	// Drive mode only (nil in Mock mode). driveAuth owns the signed-in
	// user's tokens; drive is the Drive API client using those tokens.
	drive     *DriveClient
	driveAuth *DriveAuth
}

func NewServer(cfg Config) (*Server, error) {
	store, err := NewStore(cfg.CacheDir)
	if err != nil {
		return nil, err
	}
	s := &Server{cfg: cfg, store: store, oauth: NewOAuthGuard()}
	if cfg.Provider == "drive" {
		clientID, clientSecret, err := resolveOAuthClient(cfg.Credentials)
		if err != nil {
			return nil, err
		}
		auth := newDriveAuth(clientID, clientSecret, newPlatformTokenStore())
		s.driveAuth = auth
		s.drive = newDriveClient(auth.GetAccessToken)
		// Best-effort automatic reconnect: if this PC already has a
		// stored refresh token, mint an access token now so the Hub
		// starts authenticated. Failure here is NOT fatal — the user
		// signs in via Settings → Connect Drive. EnsureAccount also
		// restores the connected-account display (from the persisted
		// email, or one userinfo lookup), so reopening the plugin
		// window never asks for sign-in again while a token exists.
		_, _ = auth.GetAccessToken()
		auth.EnsureAccount()
	} else if cfg.Provider != "mock" && cfg.Provider != "" {
		return nil, fmt.Errorf("unknown drive provider %q (use mock or drive)", cfg.Provider)
	}
	return s, nil
}

func (s *Server) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/health", s.handleHealth)
	mux.HandleFunc("/files", s.handleFiles)
	mux.HandleFunc("/files/", s.handleDeleteFile)
	mux.HandleFunc("/invites", s.handleInvites)
	mux.HandleFunc("/uploads", s.handleUpload)
	mux.HandleFunc("/downloads/", s.handleDownload)
	mux.HandleFunc("/transfers", s.handleTransfers)
	mux.HandleFunc("/events", s.handleEvents)
	mux.HandleFunc("/auth/status", s.handleAuthStatus)
	mux.HandleFunc("/auth/start", s.handleAuthStart)
	mux.HandleFunc("/auth/disconnect", s.handleAuthDisconnect)
	return cors(mux)
}

// cors allows browser/plugin clients on the same machine. The listener is
// loopback-only, so this does not expose the Hub to the network; it only
// lets the Android emulator (10.0.2.2 -> host loopback) and local web
// clients call it.
func cors(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Access-Control-Allow-Origin", "*")
		w.Header().Set("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS")
		w.Header().Set("Access-Control-Allow-Headers", "Content-Type")
		if r.Method == http.MethodOptions {
			w.WriteHeader(http.StatusNoContent)
			return
		}
		next.ServeHTTP(w, r)
	})
}

func (s *Server) handleHealth(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		writeError(w, http.StatusMethodNotAllowed, "invalid_request", "method %s not allowed on /health", r.Method)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"ok": true, "status": "ok", "provider": s.cfg.Provider, "version": "0.1.0",
	})
}

func (s *Server) handleFiles(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		writeError(w, http.StatusMethodNotAllowed, "invalid_request", "method %s not allowed on /files", r.Method)
		return
	}
	if s.drive != nil {
		files, err := s.drive.ListAll()
		if err != nil {
			writeError(w, http.StatusUnauthorized, "not_authenticated", "Drive list failed: %v", err)
			return
		}
		if files == nil {
			files = []OnorabFile{}
		}
		writeJSON(w, http.StatusOK, files)
		return
	}
	writeJSON(w, http.StatusOK, s.store.listFiles())
}

// handleDeleteFile deletes one file by id. It accepts DELETE
// /files/{id} (for API clients) and POST /files/{id}/delete (for the
// JUCE plugin, whose URL client has no proven DELETE verb — the POST
// form is the one the plugin actually uses). The deleted file's local
// cache copy is removed by Store.deleteFile; the plugin additionally
// deletes its own cache copy on success. Unknown ids are 404, never a
// silent success.
func (s *Server) handleDeleteFile(w http.ResponseWriter, r *http.Request) {
	path := strings.TrimPrefix(r.URL.Path, "/files/")
	isPostDelete := r.Method == http.MethodPost && strings.HasSuffix(path, "/delete")
	if r.Method != http.MethodDelete && !isPostDelete {
		writeError(w, http.StatusMethodNotAllowed, "invalid_request", "method %s not allowed on /files/{id}", r.Method)
		return
	}
	id := strings.TrimSuffix(path, "/delete")
	if id == "" || strings.Contains(id, "/") {
		writeError(w, http.StatusBadRequest, "invalid_request", "a file id is required in /files/{id}")
		return
	}
	if s.drive != nil {
		if err := s.drive.Delete(id); err != nil {
			if strings.Contains(err.Error(), "not_found") {
				writeError(w, http.StatusNotFound, "not_found", "no file with id %q", id)
			} else {
				writeError(w, http.StatusUnauthorized, "not_authenticated", "Drive delete failed: %v", err)
			}
			return
		}
		writeJSON(w, http.StatusOK, OnorabFile{ID: id})
		return
	}
	file, ok := s.store.deleteFile(id)
	if !ok {
		writeError(w, http.StatusNotFound, "not_found", "no file with id %q", id)
		return
	}
	writeJSON(w, http.StatusOK, file)
}

// handleInvites stores and lists project invites (Mock only, see
// OnorabInvite). POST body: {"email": "...", "project": "..."}.
// Email must contain '@'; project is sanitised like an upload project.
func (s *Server) handleInvites(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case http.MethodGet:
		s.store.mu.Lock()
		out := append([]OnorabInvite(nil), s.store.invites...)
		s.store.mu.Unlock()
		if out == nil {
			out = []OnorabInvite{}
		}
		writeJSON(w, http.StatusOK, out)
	case http.MethodPost:
		var body struct {
			Email   string `json:"email"`
			Project string `json:"project"`
		}
		if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
			writeError(w, http.StatusBadRequest, "invalid_invite", "invite body must be JSON: %v", err)
			return
		}
		email := strings.TrimSpace(body.Email)
		project := sanitiseProjectName(body.Project)
		if !strings.Contains(email, "@") || project == "" {
			writeError(w, http.StatusBadRequest, "invalid_invite", "invite needs an email containing '@' and a project name")
			return
		}
		status := "pending"
		if s.drive != nil {
			if _, err := s.drive.Share(project, email); err != nil {
				writeError(w, http.StatusUnauthorized, "not_authenticated", "Drive share failed: %v", err)
				return
			}
			// Drive permission created: the invite is real, not just
			// recorded. (The invitee's own Hub lists the shared
			// project once Drive shows it in their shared view.)
			status = "accepted"
		}
		invite := OnorabInvite{Email: email, Project: project, Status: status, CreatedAt: nowISO()}
		s.store.mu.Lock()
		s.store.invites = append(s.store.invites, invite)
		s.store.mu.Unlock()
		writeJSON(w, http.StatusCreated, invite)
	default:
		writeError(w, http.StatusMethodNotAllowed, "invalid_request", "method %s not allowed on /invites", r.Method)
	}
}

func (s *Server) handleTransfers(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		writeError(w, http.StatusMethodNotAllowed, "invalid_request", "method %s not allowed on /transfers", r.Method)
		return
	}
	writeJSON(w, http.StatusOK, s.store.listTransfers())
}

func (s *Server) handleAuthStatus(w http.ResponseWriter, r *http.Request) {
	if s.driveAuth != nil {
		writeJSON(w, http.StatusOK, map[string]any{
			"provider": s.cfg.Provider, "authenticated": s.driveAuth.Authenticated(),
			"account": s.driveAuth.Account(),
			"oauthInProgress": s.oauth.InProgress(),
			// Last sign-in failure in plain words ("" when none):
			// the plugin shows it with a "Try again" button instead
			// of a silent authenticated=false.
			"authError": s.driveAuth.LastAuthError(),
			"note":      "Real Google Drive (drive.file only). Each PC signs in with its owner's Gmail; tokens stay in that PC's OS keychain.",
		})
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"provider": s.cfg.Provider, "authenticated": false,
		"oauthInProgress": s.oauth.InProgress(),
		"note":            "Mock Drive Provider; no Google account is connected. Start the Hub with ONORAB_DRIVE_PROVIDER=drive to use real Drive.",
	})
}

// Project folder model (user decision, 2026-11-03; Phase 2 spec, Mock
// implementation here): every Onorab file belongs to a project, and on
// real Drive a project lives under the single dedicated folder
// "Onorab_projet/<project>/" — created on the FIRST upload if it does
// not exist, then reused, never recreated per upload, and no other Drive
// folder is ever touched. In the Mock provider the project is carried
// as the file's ProjectID (the protocol's existing projectId field);
// the real Drive provider must translate that same ProjectID into the
// Onorab_projet/<project>/ folder path. A project name is a plain
// display name (e.g. "marcus music"), never a path: separators are
// stripped by sanitiseProjectName so "../" tricks cannot escape the
// dedicated folder.
func sanitiseProjectName(s string) string {
	s = strings.TrimSpace(s)
	s = strings.ReplaceAll(s, "/", "-")
	s = strings.ReplaceAll(s, "\\", "-")
	s = strings.TrimSpace(s)
	if s == "." || s == ".." {
		return ""
	}
	if len(s) > 100 {
		s = s[:100]
	}
	return s
}

// driveFolderFor returns the Drive folder path for a project, for
// documentation and for the future real Drive provider. The Mock
// provider stores only the ProjectID; it does not create directories.
func driveFolderFor(project string) string {
	if project == "" {
		return "Onorab_projet"
	}
	return "Onorab_projet/" + project
}

// handleUpload accepts multipart/form-data with a single field named
// "file" (the contract used by both the VST3 plugin and the mobile app),
// plus an optional text field named "project" carrying the project
// name (see the project folder model above). It stores the bytes in
// the local cache, indexes the file, and records a completed upload
// transfer. Errors are returned as JSON, never as a silent 200 with
// no file.
func (s *Server) handleUpload(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeError(w, http.StatusMethodNotAllowed, "invalid_request", "method %s not allowed on /uploads", r.Method)
		return
	}
	if err := r.ParseMultipartForm(64 << 20); err != nil {
		writeError(w, http.StatusBadRequest, "invalid_upload", "could not parse multipart upload: %v", err)
		return
	}
	fh, header, err := r.FormFile("file")
	if err != nil {
		writeError(w, http.StatusBadRequest, "invalid_upload", "upload requires a multipart field named \"file\": %v", err)
		return
	}
	defer fh.Close()
	name := filepath.Base(header.Filename)
	if name == "." || name == "/" || name == "" {
		writeError(w, http.StatusBadRequest, "invalid_upload", "uploaded file has no usable name")
		return
	}
	if s.drive != nil {
		project := sanitiseProjectName(r.FormValue("project"))
		data, err := io.ReadAll(fh)
		if err != nil {
			writeError(w, http.StatusInternalServerError, "internal", "could not read upload: %v", err)
			return
		}
		mime := header.Header.Get("Content-Type")
		if mime == "" {
			mime = "audio/wav"
		}
		transfer := &OnorabTransfer{ID: newID("tr"), FileName: name, Direction: "upload", Status: "queued", Progress: 0}
		s.store.recordTransfer(transfer)
		if err := transfer.transition("uploading"); err != nil {
			s.store.finishTransfer(transfer, err)
			writeError(w, http.StatusInternalServerError, "internal", "%v", err)
			return
		}
		file, err := s.drive.Upload(project, name, data, mime)
		if err != nil {
			s.store.finishTransfer(transfer, err)
			writeError(w, http.StatusUnauthorized, "not_authenticated", "Drive upload failed: %v", err)
			return
		}
		transfer.FileID = file.ID
		s.store.finishTransfer(transfer, nil)
		s.store.events.Broadcast(OnorabEvent{Type: "file.added", Timestamp: nowISO(), File: &file})
		writeJSON(w, http.StatusCreated, file)
		return
	}
	id := newID("file")
	transfer := &OnorabTransfer{ID: newID("tr"), FileID: id, FileName: name, Direction: "upload", Status: "queued", Progress: 0}
	s.store.recordTransfer(transfer)
	if err := transfer.transition("uploading"); err != nil {
		s.store.finishTransfer(transfer, err)
		writeError(w, http.StatusInternalServerError, "internal", "%v", err)
		return
	}
	dst := filepath.Join(s.store.cacheDir, id+"_"+name)
	out, err := os.Create(dst)
	if err != nil {
		s.store.finishTransfer(transfer, err)
		writeError(w, http.StatusInternalServerError, "internal", "could not store upload: %v", err)
		return
	}
	size, err := io.Copy(out, fh)
	_ = out.Close()
	if err != nil {
		_ = os.Remove(dst)
		s.store.finishTransfer(transfer, err)
		writeError(w, http.StatusInternalServerError, "internal", "could not store upload: %v", err)
		return
	}
	mime := header.Header.Get("Content-Type")
	if mime == "" {
		mime = "audio/wav"
	}
	var projectID *string
	if p := sanitiseProjectName(r.FormValue("project")); p != "" {
		projectID = &p
	}
	file := OnorabFile{ID: id, Name: name, MimeType: mime, SizeBytes: size, ModifiedTime: nowISO(), ProjectID: projectID, Source: "phone"}
	s.store.mu.Lock()
	s.store.files[id] = file
	s.store.fileOrder = append(s.store.fileOrder, id)
	s.store.mu.Unlock()
	s.store.finishTransfer(transfer, nil)
	s.store.events.Broadcast(OnorabEvent{Type: "file.added", Timestamp: nowISO(), File: &file})
	writeJSON(w, http.StatusCreated, file)
}

func (s *Server) handleDownload(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		writeError(w, http.StatusMethodNotAllowed, "invalid_request", "method %s not allowed on /downloads", r.Method)
		return
	}
	id := strings.TrimPrefix(r.URL.Path, "/downloads/")
	if id == "" || strings.Contains(id, "/") {
		writeError(w, http.StatusBadRequest, "invalid_request", "a file id is required in /downloads/{fileId}")
		return
	}
	if s.drive != nil {
		meta, err := s.drive.GetFile(id)
		if err != nil {
			if strings.Contains(err.Error(), "not_found") {
				writeError(w, http.StatusNotFound, "not_found", "no file with id %q", id)
			} else {
				writeError(w, http.StatusUnauthorized, "not_authenticated", "Drive download failed: %v", err)
			}
			return
		}
		transfer := &OnorabTransfer{ID: newID("tr"), FileID: id, FileName: meta.Name, Direction: "download", Status: "queued", Progress: 0}
		s.store.recordTransfer(transfer)
		_ = transfer.transition("downloading")
		data, err := s.drive.Download(id)
		if err != nil {
			s.store.finishTransfer(transfer, err)
			writeError(w, http.StatusInternalServerError, "internal", "%v", err)
			return
		}
		w.Header().Set("Content-Type", meta.MimeType)
		w.Header().Set("Content-Disposition", "attachment; filename="+meta.Name)
		w.Header().Set("Content-Length", fmt.Sprint(len(data)))
		if _, err := w.Write(data); err != nil {
			s.store.finishTransfer(transfer, err)
			return
		}
		s.store.finishTransfer(transfer, nil)
		return
	}
	s.store.mu.Lock()
	file, ok := s.store.files[id]
	s.store.mu.Unlock()
	if !ok {
		writeError(w, http.StatusNotFound, "not_found", "no file with id %q", id)
		return
	}
	transfer := &OnorabTransfer{ID: newID("tr"), FileID: id, FileName: file.Name, Direction: "download", Status: "queued", Progress: 0}
	s.store.recordTransfer(transfer)
	_ = transfer.transition("downloading")
	data, err := s.store.bytesFor(id, file)
	if err != nil {
		s.store.finishTransfer(transfer, err)
		writeError(w, http.StatusInternalServerError, "internal", "%v", err)
		return
	}
	w.Header().Set("Content-Type", file.MimeType)
	w.Header().Set("Content-Disposition", "attachment; filename="+file.Name)
	w.Header().Set("Content-Length", fmt.Sprint(len(data)))
	if _, err := w.Write(data); err != nil {
		s.store.finishTransfer(transfer, err)
		return
	}
	s.store.finishTransfer(transfer, nil)
}
