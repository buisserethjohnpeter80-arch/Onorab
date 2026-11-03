package main

import (
	"encoding/binary"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"strings"
)

// Mock Drive Provider (Phase 1).
//
// The three seeded files have no stored bytes: their downloads are
// generated deterministically as real WAV files (mono, 16-bit, 44.1 kHz
// sine tones), so preview and download work end-to-end without Google.
// Uploaded files are stored as real bytes in the cache directory and
// served back verbatim.

// generateToneWAV returns a valid WAV file: `seconds` seconds of a sine
// tone at `freqHz`. Used only for mock downloads and tests.
func generateToneWAV(freqHz float64, seconds int) []byte {
	rate := 44100
	frames := rate * seconds
	dataSize := frames * 2 // mono 16-bit
	buf := make([]byte, 44+dataSize)
	copy(buf[0:], "RIFF")
	binary.LittleEndian.PutUint32(buf[4:], uint32(36+dataSize))
	copy(buf[8:], "WAVE")
	copy(buf[12:], "fmt ")
	binary.LittleEndian.PutUint32(buf[16:], 16)
	binary.LittleEndian.PutUint16(buf[20:], 1) // PCM
	binary.LittleEndian.PutUint16(buf[22:], 1) // mono
	binary.LittleEndian.PutUint32(buf[24:], uint32(rate))
	binary.LittleEndian.PutUint32(buf[28:], uint32(rate*2))
	binary.LittleEndian.PutUint16(buf[32:], 2)
	binary.LittleEndian.PutUint16(buf[34:], 16)
	copy(buf[36:], "data")
	binary.LittleEndian.PutUint32(buf[40:], uint32(dataSize))
	for i := 0; i < frames; i++ {
		sample := int16(0.5 * 32767 * math.Sin(2*math.Pi*freqHz*float64(i)/float64(rate)))
		binary.LittleEndian.PutUint16(buf[44+i*2:], uint16(sample))
	}
	return buf
}

func mockToneFor(id string) ([]byte, bool) {
	switch id {
	case "mock_voice_memo":
		return generateToneWAV(440, 1), true
	case "mock_beat_idea":
		return generateToneWAV(220, 2), true
	case "mock_fl_capture":
		return generateToneWAV(330, 1), true
	}
	return nil, false
}

// bytesFor returns download bytes for a file: generated tones for mock
// seeds, cached bytes for uploads. It never invents bytes for an unknown
// id — callers have already verified the id exists in the index.
func (s *Store) bytesFor(id string, file OnorabFile) ([]byte, error) {
	if data, ok := mockToneFor(id); ok {
		return data, nil
	}
	entries, err := os.ReadDir(s.cacheDir)
	if err != nil {
		return nil, fmt.Errorf("read cache dir: %w", err)
	}
	for _, e := range entries {
		if strings.HasPrefix(e.Name(), id+"_") {
			data, err := os.ReadFile(filepath.Join(s.cacheDir, e.Name()))
			if err != nil {
				return nil, fmt.Errorf("read cached file: %w", err)
			}
			return data, nil
		}
	}
	return nil, fmt.Errorf("no cached bytes for file %q (mock tones cover only seeded files)", file.Name)
}
