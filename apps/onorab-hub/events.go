package main

import (
	"bufio"
	"crypto/sha1"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"strings"
	"sync"
)

// EventHub broadcasts OnorabEvents to WebSocket /events clients.
//
// Minimal, dependency-free WebSocket implementation (RFC 6455, server
// side only): handshake, unmasked text frames to clients, masked frame
// parsing from clients (control frames and text are read and discarded;
// only server->client events carry data). This keeps the Hub on the Go
// standard library alone, so `go test` and CI need no module downloads.

// EventHub is a set of connected subscribers.
type EventHub struct {
	mu   sync.Mutex
	subs map[chan OnorabEvent]struct{}
}

func NewEventHub() *EventHub { return &EventHub{subs: map[chan OnorabEvent]struct{}{}} }

func (h *EventHub) Subscribe() chan OnorabEvent {
	ch := make(chan OnorabEvent, 32)
	h.mu.Lock()
	h.subs[ch] = struct{}{}
	h.mu.Unlock()
	return ch
}

func (h *EventHub) Unsubscribe(ch chan OnorabEvent) {
	h.mu.Lock()
	delete(h.subs, ch)
	h.mu.Unlock()
}

// Broadcast sends to every subscriber without blocking the caller: a
// slow client drops events instead of stalling uploads/downloads.
func (h *EventHub) Broadcast(ev OnorabEvent) {
	h.mu.Lock()
	defer h.mu.Unlock()
	for ch := range h.subs {
		select {
		case ch <- ev:
		default:
		}
	}
}

const wsGUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

func wsAcceptKey(key string) string {
	sum := sha1.Sum([]byte(key + wsGUID))
	return base64.StdEncoding.EncodeToString(sum[:])
}

// writeTextFrame writes one unmasked server text frame.
func writeTextFrame(w io.Writer, payload []byte) error {
	header := []byte{0x81} // FIN + text opcode
	switch {
	case len(payload) < 126:
		header = append(header, byte(len(payload)))
	case len(payload) < 65536:
		header = append(header, 126, byte(len(payload)>>8), byte(len(payload)))
	default:
		header = append(header, 127)
		for i := 7; i >= 0; i-- {
			header = append(header, byte(len(payload)>>(8*i)))
		}
	}
	if _, err := w.Write(header); err != nil {
		return err
	}
	_, err := w.Write(payload)
	return err
}

// readClientFrame reads one client frame (masked, per RFC 6455) and
// returns its opcode and payload. Close frames (0x8) end the connection.
func readClientFrame(r *bufio.Reader) (opcode byte, payload []byte, err error) {
	b0, err := r.ReadByte()
	if err != nil {
		return 0, nil, err
	}
	b1, err := r.ReadByte()
	if err != nil {
		return 0, nil, err
	}
	opcode = b0 & 0x0F
	masked := b1&0x80 != 0
	length := int64(b1 & 0x7F)
	switch length {
	case 126:
		var ext [2]byte
		if _, err := io.ReadFull(r, ext[:]); err != nil {
			return 0, nil, err
		}
		length = int64(binary.BigEndian.Uint16(ext[:]))
	case 127:
		var ext [8]byte
		if _, err := io.ReadFull(r, ext[:]); err != nil {
			return 0, nil, err
		}
		length = int64(binary.BigEndian.Uint64(ext[:]))
	}
	var mask [4]byte
	if masked {
		if _, err := io.ReadFull(r, mask[:]); err != nil {
			return 0, nil, err
		}
	}
	payload = make([]byte, length)
	if _, err := io.ReadFull(r, payload); err != nil {
		return 0, nil, err
	}
	if masked {
		for i := range payload {
			payload[i] ^= mask[i%4]
		}
	}
	return opcode, payload, nil
}

func (s *Server) handleEvents(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		writeError(w, http.StatusMethodNotAllowed, "invalid_request", "method %s not allowed on /events", r.Method)
		return
	}
	key := r.Header.Get("Sec-WebSocket-Key")
	if key == "" || !strings.EqualFold(r.Header.Get("Upgrade"), "websocket") {
		writeError(w, http.StatusBadRequest, "invalid_request", "/events requires a WebSocket upgrade (Sec-WebSocket-Key + Upgrade: websocket)")
		return
	}
	hj, ok := w.(http.Hijacker)
	if !ok {
		writeError(w, http.StatusInternalServerError, "internal", "server does not support connection hijacking")
		return
	}
	conn, rw, err := hj.Hijack()
	if err != nil {
		writeError(w, http.StatusInternalServerError, "internal", "hijack failed: %v", err)
		return
	}
	defer conn.Close()
	fmt.Fprintf(rw, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", wsAcceptKey(key))
	if err := rw.Flush(); err != nil {
		return
	}
	ch := s.store.events.Subscribe()
	defer s.store.events.Unsubscribe(ch)
	// Reader goroutine: consume client frames so pings/closes do not
	// stall; any read error ends the connection.
	done := make(chan struct{})
	go func() {
		defer close(done)
		br := bufio.NewReader(conn)
		for {
			op, payload, err := readClientFrame(br)
			if err != nil {
				return
			}
			if op == 0x8 {
				return
			}
			if op == 0x9 { // ping -> pong
				frame := append([]byte{0x8A, byte(len(payload))}, payload...)
				if _, err := conn.Write(frame); err != nil {
					return
				}
			}
		}
	}()
	// Send a connected event immediately so clients can confirm the
	// stream is live without waiting for a transfer.
	_ = writeTextFrame(conn, mustJSON(OnorabEvent{Type: "hub.connected", Timestamp: nowISO()}))
	for {
		select {
		case <-done:
			return
		case ev := <-ch:
			if err := writeTextFrame(conn, mustJSON(ev)); err != nil {
				return
			}
		}
	}
}

func mustJSON(v any) []byte {
	b, err := json.Marshal(v)
	if err != nil {
		panic(fmt.Sprintf("onorab-hub: marshal event failed: %v", err))
	}
	return b
}
