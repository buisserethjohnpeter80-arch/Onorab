//go:build !windows

package main

import "errors"

// Non-Windows builds have no OS keychain store: token persistence is
// Windows-only in the shipped product (the VST + Hub are Windows
// software). Tests use MemoryTokenStore, never this stub.
type errorTokenStore struct{}

func newPlatformTokenStore() TokenStore { return &errorTokenStore{} }

func (s *errorTokenStore) Save(account, refreshToken string) error {
	return errors.New("OS keychain token storage is available on Windows only")
}

func (s *errorTokenStore) Load(account string) (string, error) {
	return "", errors.New("OS keychain token storage is available on Windows only")
}

func (s *errorTokenStore) Delete(account string) error {
	return errors.New("OS keychain token storage is available on Windows only")
}
