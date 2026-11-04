// Package main is the Onorab Hub: a localhost-only HTTP + WebSocket service
// that sits between the Onorab VST3 plugin, the mobile app (via the host
// loopback), and a drive provider.
//
// Providers: "mock" (Phase 1 seeds, no Google) or "drive" (real Google
// Drive, drive.file only, per-PC user sign-in — see drive_oauth.go and
// docs/architecture.md §3.4 for the commercial multi-PC model).
//
// Security rules enforced here, not just documented:
//   - The listener binds 127.0.0.1 only (never 0.0.0.0).
//   - No token is ever written into an FL Studio project: the Hub holds
//     no project state at all, and the plugin stores only Hub URL +
//     cache directory in its state.
//   - The release build carries the Onorab Desktop OAuth client id +
//     secret (CI secrets, ldflags-injected, never in the repo). For a
//     Google Desktop app the secret is not a confidential secret — it
//     ships inside the app by design and grants nothing on its own —
//     so no backend proxy is needed. What must stay private, each
//     user's refresh token, is never in the binary or the repo: it
//     lives only in that PC's Windows Credential Manager.
//   - An unknown provider fails fast instead of silently falling back
//     to mock data, which would mislead the user into thinking real
//     Drive sync was working.
package main

import (
	"flag"
	"io"
	"log"
	"net/http"
	"os"
	"strconv"
)

// Config is everything the Hub reads from the environment. See .env.example
// and config.example.json at the repo root for the documented values.
type Config struct {
	Port        int
	Provider    string
	CacheDir    string
	Credentials string
}

// loadConfig applies defaults, then flag, then environment overrides.
// The -provider flag exists so the plugin can auto-launch the installed
// Hub in Drive mode WITHOUT env vars, PowerShell, or any setup on the
// musician's PC: `onorab-hub.exe -provider drive`. Precedence:
// flag > environment > default (mock). It never reads a credential
// file here; that happens only in the OAuth path (Phase 2).
func loadConfig() Config {
	providerFlag := flag.String("provider", "", "drive provider (mock or drive); overrides ONORAB_DRIVE_PROVIDER")
	flag.Parse()
	cfg := Config{Port: 18741, Provider: "mock"}
	if v := os.Getenv("ONORAB_HUB_PORT"); v != "" {
		if p, err := strconv.Atoi(v); err == nil && p > 0 && p < 65536 {
			cfg.Port = p
		} else {
			log.Printf("onorab-hub: ignoring invalid ONORAB_HUB_PORT %q, using %d", v, cfg.Port)
		}
	}
	if v := os.Getenv("ONORAB_DRIVE_PROVIDER"); v != "" {
		cfg.Provider = v
	}
	if *providerFlag != "" {
		cfg.Provider = *providerFlag
	}
	if v := os.Getenv("ONORAB_CACHE_DIR"); v != "" {
		cfg.CacheDir = v
	} else if dir, err := os.UserCacheDir(); err == nil {
		cfg.CacheDir = dir + string(os.PathSeparator) + "Onorab" + string(os.PathSeparator) + "cache"
	} else {
		cfg.CacheDir = os.TempDir() + string(os.PathSeparator) + "onorab-cache"
	}
	cfg.Credentials = os.Getenv("ONORAB_CREDENTIALS_PATH")
	return cfg
}

func main() {
	cfg := loadConfig()
	setupFileLogging()
	if cfg.Provider != "mock" && cfg.Provider != "drive" {
		log.Fatalf("onorab-hub: unknown drive provider %q (use mock or drive)", cfg.Provider)
	}
	hub, err := NewServer(cfg)
	if err != nil {
		log.Fatalf("onorab-hub: %v", err)
	}
	addr := "127.0.0.1:" + strconv.Itoa(cfg.Port)
	log.Printf("onorab-hub: listening on %s (localhost only), provider=%s, cache=%s", addr, cfg.Provider, cfg.CacheDir)
	if err := http.ListenAndServe(addr, hub.Handler()); err != nil {
		log.Fatalf("onorab-hub: listen failed: %v", err)
	}
}

// setupFileLogging mirrors Hub logs to %LOCALAPPDATA%\Onorab\hub.log
// (next to the cache dir) in addition to stderr. An auto-launched Hub
// has no console window (the plugin launches it with CREATE_NO_WINDOW
// via JUCE ChildProcess), so without this file its logs would be
// invisible on the musician's PC. Best-effort: any failure here leaves
// stderr-only logging. The file is appended for the process lifetime
// and never rotated in this version; it grows by one line per log
// event. It never contains tokens: the Hub never logs one.
func setupFileLogging() {
	dir, err := os.UserCacheDir()
	if err != nil {
		return
	}
	logDir := dir + string(os.PathSeparator) + "Onorab"
	if err := os.MkdirAll(logDir, 0o755); err != nil {
		return
	}
	f, err := os.OpenFile(logDir+string(os.PathSeparator)+"hub.log",
		os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o644)
	if err != nil {
		return
	}
	log.SetOutput(io.MultiWriter(os.Stderr, f))
}
