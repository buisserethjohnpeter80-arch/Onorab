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
//   - No OAuth client secret is compiled in. The release build injects
//     only the client id via -X main.buildOAuthClientID (CI secret);
//     a Desktop OAuth JSON at ONORAB_CREDENTIALS_PATH is dev-only.
//   - An unknown provider fails fast instead of silently falling back
//     to mock data, which would mislead the user into thinking real
//     Drive sync was working.
package main

import (
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

// loadConfig applies defaults, then environment overrides. It never reads
// a credential file here; that happens only in the OAuth path (Phase 2).
func loadConfig() Config {
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
