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
	"sort"
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
type Store struct {
	mu        sync.Mutex
	files     map[string]OnorabFile
	transfers map[string]*OnorabTransfer
	order     []string // transfer insertion order
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
	}
}

func (s *Store) listFiles() []OnorabFile {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := make([]OnorabFile, 0, len(s.files))
	for _, f := range s.files {
		out = append(out, f)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Name < out[j].Name })
	return out
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
}

func NewServer(cfg Config) (*Server, error) {
	store, err := NewStore(cfg.CacheDir)
	if err != nil {
		return nil, err
	}
	return &Server{cfg: cfg, store: store, oauth: NewOAuthGuard()}, nil
}

func (s *Server) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/health", s.handleHealth)
	mux.HandleFunc("/files", s.handleFiles)
	mux.HandleFunc("/uploads", s.handleUpload)
	mux.HandleFunc("/downloads/", s.handleDownload)
	mux.HandleFunc("/transfers", s.handleTransfers)
	mux.HandleFunc("/events", s.handleEvents)
	mux.HandleFunc("/auth/status", s.handleAuthStatus)
	mux.HandleFunc("/auth/start", s.handleAuthStart)
	return cors(mux)
}

// cors allows browser/plugin clients on the same machine. The listener is
// loopback-only, so this does not expose the Hub to the network; it only
// lets the Android emulator (10.0.2.2 -> host loopback) and local web
// clients call it.
func cors(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Access-Control-Allow-Origin", "*")
		w.Header().Set("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
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
	writeJSON(w, http.StatusOK, s.store.listFiles())
}

func (s *Server) handleTransfers(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		writeError(w, http.StatusMethodNotAllowed, "invalid_request", "method %s not allowed on /transfers", r.Method)
		return
	}
	writeJSON(w, http.StatusOK, s.store.listTransfers())
}

func (s *Server) handleAuthStatus(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, map[string]any{
		"provider": s.cfg.Provider, "authenticated": false,
		"oauthInProgress": s.oauth.InProgress(),
		"note":            "Phase 1 uses the Mock Drive Provider; no Google account is connected.",
	})
}

// handleUpload accepts multipart/form-data with a single field named
// "file" (the contract used by both the VST3 plugin and the mobile app).
// It stores the bytes in the local cache, indexes the file, and records
// a completed upload transfer. Errors are returned as JSON, never as a
// silent 200 with no file.
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
	file := OnorabFile{ID: id, Name: name, MimeType: mime, SizeBytes: size, ModifiedTime: nowISO(), ProjectID: nil, Source: "phone"}
	s.store.mu.Lock()
	s.store.files[id] = file
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
