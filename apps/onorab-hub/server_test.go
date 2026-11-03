package main

import (
	"bufio"
	"bytes"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"io"
	"mime/multipart"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
)

func testServer(t *testing.T) *Server {
	t.Helper()
	srv, err := NewServer(Config{Port: 18741, Provider: "mock", CacheDir: t.TempDir()})
	if err != nil {
		t.Fatalf("NewServer: %v", err)
	}
	return srv
}

func TestHealth(t *testing.T) {
	srv := testServer(t)
	rec := httptest.NewRecorder()
	srv.Handler().ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/health", nil))
	if rec.Code != http.StatusOK {
		t.Fatalf("health status = %d, want 200", rec.Code)
	}
	var body map[string]any
	if err := json.Unmarshal(rec.Body.Bytes(), &body); err != nil {
		t.Fatalf("health body is not JSON: %v", err)
	}
	if body["ok"] != true || body["provider"] != "mock" {
		t.Fatalf("health body = %v, want ok=true provider=mock", body)
	}
}

func TestFilesSeededAndShape(t *testing.T) {
	srv := testServer(t)
	rec := httptest.NewRecorder()
	srv.Handler().ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/files", nil))
	if rec.Code != http.StatusOK {
		t.Fatalf("files status = %d", rec.Code)
	}
	var files []OnorabFile
	if err := json.Unmarshal(rec.Body.Bytes(), &files); err != nil {
		t.Fatalf("files body: %v", err)
	}
	if len(files) != 3 {
		t.Fatalf("seeded files = %d, want 3", len(files))
	}
	for _, f := range files {
		if f.ID == "" || f.Name == "" || f.SizeBytes <= 0 || f.MimeType == "" || f.ModifiedTime == "" {
			t.Fatalf("file missing required protocol fields: %+v", f)
		}
	}
}

func TestUploadDownloadRoundTrip(t *testing.T) {
	srv := testServer(t)
	h := srv.Handler()
	var buf bytes.Buffer
	mw := multipart.NewWriter(&buf)
	part, err := mw.CreateFormFile("file", "capture-test.wav")
	if err != nil {
		t.Fatal(err)
	}
	original := generateToneWAV(440, 1)
	if _, err := part.Write(original); err != nil {
		t.Fatal(err)
	}
	if err := mw.Close(); err != nil {
		t.Fatal(err)
	}
	req := httptest.NewRequest(http.MethodPost, "/uploads", &buf)
	req.Header.Set("Content-Type", mw.FormDataContentType())
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	if rec.Code != http.StatusCreated {
		t.Fatalf("upload status = %d, body = %s", rec.Code, rec.Body.String())
	}
	var uploaded OnorabFile
	if err := json.Unmarshal(rec.Body.Bytes(), &uploaded); err != nil {
		t.Fatalf("upload response: %v", err)
	}
	if uploaded.SizeBytes != int64(len(original)) {
		t.Fatalf("uploaded size = %d, want %d", uploaded.SizeBytes, len(original))
	}
	rec = httptest.NewRecorder()
	h.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/downloads/"+uploaded.ID, nil))
	if rec.Code != http.StatusOK {
		t.Fatalf("download status = %d, body = %s", rec.Code, rec.Body.String())
	}
	if !bytes.Equal(rec.Body.Bytes(), original) {
		t.Fatalf("downloaded bytes differ from uploaded bytes")
	}
}

func TestUploadMissingFileFieldIsErrorNotSilentSuccess(t *testing.T) {
	srv := testServer(t)
	var buf bytes.Buffer
	mw := multipart.NewWriter(&buf)
	_ = mw.WriteField("wrong", "x")
	_ = mw.Close()
	req := httptest.NewRequest(http.MethodPost, "/uploads", &buf)
	req.Header.Set("Content-Type", mw.FormDataContentType())
	rec := httptest.NewRecorder()
	srv.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400", rec.Code)
	}
	var body ErrorBody
	if err := json.Unmarshal(rec.Body.Bytes(), &body); err != nil || body.Error.Code != "invalid_upload" {
		t.Fatalf("body = %s, want error code invalid_upload", rec.Body.String())
	}
}

func TestDownloadUnknownFileIs404(t *testing.T) {
	srv := testServer(t)
	rec := httptest.NewRecorder()
	srv.Handler().ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/downloads/nope", nil))
	if rec.Code != http.StatusNotFound {
		t.Fatalf("status = %d, want 404", rec.Code)
	}
}

func TestMockDownloadIsValidWAV(t *testing.T) {
	srv := testServer(t)
	rec := httptest.NewRecorder()
	srv.Handler().ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/downloads/mock_voice_memo", nil))
	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d", rec.Code)
	}
	b := rec.Body.Bytes()
	if len(b) < 44 || string(b[0:4]) != "RIFF" || string(b[8:12]) != "WAVE" {
		t.Fatalf("mock download is not a WAV file (first bytes %q)", b[:12])
	}
}

func TestTransfersRecordedForUploadAndDownload(t *testing.T) {
	srv := testServer(t)
	h := srv.Handler()
	// Trigger a download transfer.
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/downloads/mock_beat_idea", nil))
	if rec.Code != http.StatusOK {
		t.Fatalf("download status = %d", rec.Code)
	}
	rec = httptest.NewRecorder()
	h.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/transfers", nil))
	var transfers []OnorabTransfer
	if err := json.Unmarshal(rec.Body.Bytes(), &transfers); err != nil {
		t.Fatalf("transfers: %v", err)
	}
	if len(transfers) != 1 || transfers[0].Status != "completed" || transfers[0].Direction != "download" || transfers[0].Progress != 100 {
		t.Fatalf("transfers = %+v, want one completed download at 100", transfers)
	}
	if transfers[0].Error != nil {
		t.Fatalf("completed transfer must have null error, got %q", *transfers[0].Error)
	}
}

func TestTransferStateMachineRejectsInvalidTransitions(t *testing.T) {
	tr := &OnorabTransfer{Status: "completed"}
	if err := tr.transition("uploading"); err == nil {
		t.Fatal("completed -> uploading must be rejected")
	}
	tr2 := &OnorabTransfer{Status: "queued"}
	if err := tr2.transition("completed"); err == nil {
		t.Fatal("queued -> completed must be rejected (must go through uploading/downloading)")
	}
	if err := tr2.transition("uploading"); err != nil {
		t.Fatalf("queued -> uploading: %v", err)
	}
	if err := tr2.transition("completed"); err != nil {
		t.Fatalf("uploading -> completed: %v", err)
	}
}

func TestOAuthGuardSingleFlight(t *testing.T) {
	g := NewOAuthGuard()
	if !g.TryStart() {
		t.Fatal("first TryStart must succeed")
	}
	if g.TryStart() {
		t.Fatal("second TryStart while in progress must fail")
	}
	if !g.InProgress() {
		t.Fatal("InProgress must be true")
	}
	g.Finish()
	if !g.TryStart() {
		t.Fatal("TryStart after Finish must succeed")
	}
	g.Finish()
}

func TestOAuthGuardConcurrent(t *testing.T) {
	g := NewOAuthGuard()
	var wg sync.WaitGroup
	wins := 0
	var mu sync.Mutex
	for i := 0; i < 16; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			if g.TryStart() {
				mu.Lock()
				wins++
				mu.Unlock()
			}
		}()
	}
	wg.Wait()
	if wins != 1 {
		t.Fatalf("concurrent TryStart winners = %d, want exactly 1", wins)
	}
}

func TestPKCEChallengeIsS256(t *testing.T) {
	verifier, challenge, err := pkcePair()
	if err != nil {
		t.Fatal(err)
	}
	// S256: challenge = BASE64URL(SHA256(verifier)).
	sum := sha256.Sum256([]byte(verifier))
	want := base64.RawURLEncoding.EncodeToString(sum[:])
	if challenge != want {
		t.Fatalf("challenge = %q, want S256 %q", challenge, want)
	}
	if verifier == challenge {
		t.Fatal("challenge must differ from verifier (S256, not plain)")
	}
}

func TestAuthStartInMockModeIsClearError(t *testing.T) {
	srv := testServer(t)
	rec := httptest.NewRecorder()
	srv.Handler().ServeHTTP(rec, httptest.NewRequest(http.MethodPost, "/auth/start", nil))
	if rec.Code != http.StatusNotImplemented {
		t.Fatalf("status = %d, want 501 in mock mode", rec.Code)
	}
	var body ErrorBody
	if err := json.Unmarshal(rec.Body.Bytes(), &body); err != nil || body.Error.Code != "oauth_unavailable" {
		t.Fatalf("body = %s, want oauth_unavailable", rec.Body.String())
	}
}

func TestProtocolFieldNamesMatchMobileModels(t *testing.T) {
	// The mobile app's OnorabFile uses: id, name, mimeType, sizeBytes,
	// modifiedTime, projectId, source. Marshal a Hub file and require
	// exactly those JSON keys to be present.
	f := OnorabFile{ID: "x", Name: "a.wav", MimeType: "audio/wav", SizeBytes: 10, ModifiedTime: nowISO(), ProjectID: nil, Source: "phone"}
	b, _ := json.Marshal(f)
	for _, key := range []string{`"id"`, `"name"`, `"mimeType"`, `"sizeBytes"`, `"modifiedTime"`, `"projectId"`, `"source"`} {
		if !strings.Contains(string(b), key) {
			t.Fatalf("marshalled file %s is missing key %s", b, key)
		}
	}
}

// TestWebSocketEvents performs a real RFC 6455 handshake over a raw TCP
// connection against an httptest server, then reads the hub.connected
// frame and a broadcast file.added frame.
func TestWebSocketEvents(t *testing.T) {
	srv := testServer(t)
	ts := httptest.NewServer(srv.Handler())
	defer ts.Close()
	addr := strings.TrimPrefix(ts.URL, "http://")
	conn, err := net.Dial("tcp", addr)
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	key := base64.StdEncoding.EncodeToString([]byte("onorab-test-key-16"))
	fmt.Fprintf(conn, "GET /events HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n", addr, key)
	br := bufio.NewReader(conn)
	status, err := br.ReadString('\n')
	if err != nil || !strings.Contains(status, "101") {
		t.Fatalf("handshake status = %q, err %v", status, err)
	}
	// Read headers until blank line.
	for {
		line, err := br.ReadString('\n')
		if err != nil {
			t.Fatal(err)
		}
		if line == "\r\n" {
			break
		}
	}
	readFrame := func() (string, error) {
		b0, err := br.ReadByte()
		if err != nil {
			return "", err
		}
		if b0 != 0x81 {
			return "", fmt.Errorf("expected text frame 0x81, got 0x%x", b0)
		}
		b1, err := br.ReadByte()
		if err != nil {
			return "", err
		}
		n := int64(b1 & 0x7F)
		switch n {
		case 126:
			var ext [2]byte
			if _, err := io.ReadFull(br, ext[:]); err != nil {
				return "", err
			}
			n = int64(ext[0])<<8 | int64(ext[1])
		case 127:
			var ext [8]byte
			if _, err := io.ReadFull(br, ext[:]); err != nil {
				return "", err
			}
			n = 0
			for _, b := range ext {
				n = n<<8 | int64(b)
			}
		}
		payload := make([]byte, n)
		if _, err := io.ReadFull(br, payload); err != nil {
			return "", err
		}
		return string(payload), nil
	}
	first, err := readFrame()
	if err != nil {
		t.Fatalf("first frame: %v", err)
	}
	var ev OnorabEvent
	if err := json.Unmarshal([]byte(first), &ev); err != nil || ev.Type != "hub.connected" {
		t.Fatalf("first event = %s, want hub.connected", first)
	}
	f := OnorabFile{ID: "ws1", Name: "ws.wav", MimeType: "audio/wav", SizeBytes: 1, ModifiedTime: nowISO(), Source: "phone"}
	srv.store.events.Broadcast(OnorabEvent{Type: "file.added", Timestamp: nowISO(), File: &f})
	second, err := readFrame()
	if err != nil {
		t.Fatalf("second frame: %v", err)
	}
	if err := json.Unmarshal([]byte(second), &ev); err != nil || ev.Type != "file.added" {
		t.Fatalf("second event = %s, want file.added", second)
	}
}

func TestEventsWithoutUpgradeIsError(t *testing.T) {
	srv := testServer(t)
	rec := httptest.NewRecorder()
	srv.Handler().ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/events", nil))
	if rec.Code != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400 for plain GET /events", rec.Code)
	}
}
