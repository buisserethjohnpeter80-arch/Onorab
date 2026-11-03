//go:build windows

package main

import (
	"errors"
	"syscall"
	"unsafe"
)

// Windows Credential Manager token store (wincred).
//
// The refresh token is a generic credential named
// "Onorab:drive:<account>", persisted for the current Windows user
// (CRED_PERSIST_LOCAL_MACHINE). It is readable only by that user on
// that PC — copying the Hub to another PC does not carry the token,
// which is exactly the commercial model: every buyer signs in on
// their own machine. No token is ever written to a file by this store.

var (
	advapi32       = syscall.NewLazyDLL("advapi32.dll")
	procCredWrite  = advapi32.NewProc("CredWriteW")
	procCredRead   = advapi32.NewProc("CredReadW")
	procCredDelete = advapi32.NewProc("CredDeleteW")
	procCredFree   = advapi32.NewProc("CredFree")
)

type filetime struct {
	low  uint32
	high uint32
}

// credential mirrors CREDENTIALW on 64-bit Windows. Field order and
// widths must match the Win32 struct exactly.
type credential struct {
	flags            uint32
	typ              uint32
	targetName       *uint16
	comment          *uint16
	lastWritten      filetime
	credentialBlobSz uint32
	credentialBlob   *byte
	persist          uint32
	attributeCount   uint32
	attributes       uintptr
	targetAlias      *uint16
	userName         *uint16
}

const (
	credTypeGeneric        = 1
	credPersistLocalMachine = 2
)

type wincredTokenStore struct{}

func newPlatformTokenStore() TokenStore { return &wincredTokenStore{} }

func credTarget(account string) *uint16 {
	name, _ := syscall.UTF16PtrFromString("Onorab:drive:" + account)
	return name
}

func (s *wincredTokenStore) Save(account, refreshToken string) error {
	target := credTarget(account)
	blob := []byte(refreshToken)
	var blobPtr *byte
	if len(blob) > 0 {
		blobPtr = &blob[0]
	}
	cred := &credential{
		flags:            0,
		typ:              credTypeGeneric,
		targetName:       target,
		credentialBlobSz: uint32(len(blob)),
		credentialBlob:   blobPtr,
		persist:          credPersistLocalMachine,
	}
	ret, _, callErr := procCredWrite.Call(uintptr(unsafe.Pointer(cred)), 0)
	if ret == 0 {
		if callErr != nil && callErr != syscall.Errno(0) {
			return callErr
		}
		return errors.New("CredWriteW failed")
	}
	return nil
}

func (s *wincredTokenStore) Load(account string) (string, error) {
	target := credTarget(account)
	var credPtr *credential
	ret, _, callErr := procCredRead.Call(uintptr(unsafe.Pointer(target)), uintptr(credTypeGeneric), 0,
		uintptr(unsafe.Pointer(&credPtr)))
	if ret == 0 {
		if callErr != nil && callErr != syscall.Errno(0) {
			return "", callErr
		}
		return "", errors.New("no stored token for account")
	}
	defer procCredFree.Call(uintptr(unsafe.Pointer(credPtr)))
	if credPtr.credentialBlob == nil || credPtr.credentialBlobSz == 0 {
		return "", errors.New("stored token is empty")
	}
	blob := unsafe.Slice(credPtr.credentialBlob, credPtr.credentialBlobSz)
	out := make([]byte, len(blob))
	copy(out, blob)
	return string(out), nil
}

func (s *wincredTokenStore) Delete(account string) error {
	target := credTarget(account)
	ret, _, callErr := procCredDelete.Call(uintptr(unsafe.Pointer(target)), uintptr(credTypeGeneric), 0)
	if ret == 0 {
		if callErr != nil && callErr != syscall.Errno(0) {
			return callErr
		}
		return errors.New("CredDeleteW failed")
	}
	return nil
}
