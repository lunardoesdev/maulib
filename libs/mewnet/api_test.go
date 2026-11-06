package mewnet

import (
	"context"
	"testing"
)

func TestFingerprintIdentity(t *testing.T) {
	a := "vless://aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee@127.0.0.1:443?encryption=none&security=tls&type=ws#one"
	b := "vless://aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee@127.0.0.1:443?encryption=none&security=reality&type=grpc#two"
	c := "vless://bbbbbbbb-bbbb-cccc-dddd-eeeeeeeeeeee@127.0.0.1:443?encryption=none#three"
	if Fingerprint(a) != Fingerprint(b) {
		t.Fatal("same host/protocol/credentials must have the same identity")
	}
	if Fingerprint(a) == Fingerprint(c) {
		t.Fatal("different credentials must have different identities")
	}
	if len(FingerprintBytes(a)) != 32 {
		t.Fatal("raw fingerprint must be 32 bytes")
	}
}

func TestClientLifecycle(t *testing.T) {
	client, err := Start(Config{MixedPort: -1, LogLevel: "silent"})
	if err != nil {
		t.Fatalf("start: %v", err)
	}
	if client.Handle() == 0 {
		t.Fatal("handle is zero")
	}
	status, err := client.Status()
	if err != nil {
		t.Fatalf("status: %v", err)
	}
	if !status.Running {
		t.Fatal("core is not running")
	}
	if err := client.Close(); err != nil {
		t.Fatalf("close: %v", err)
	}
	if err := client.Close(); err != nil {
		t.Fatalf("second close: %v", err)
	}
}

func TestBatchInvalidLinks(t *testing.T) {
	client, err := Start(Config{MixedPort: -1, LogLevel: "silent"})
	if err != nil {
		t.Fatalf("start: %v", err)
	}
	defer client.Close()
	links := []string{"not-a-link", "vless://bad", "vmess://bad"}
	results := client.TestLinksBatch(context.Background(), links, DelayOptions{TimeoutMS: 100}, 3)
	if len(results) != len(links) {
		t.Fatalf("got %d results, want %d", len(results), len(links))
	}
	for _, result := range results {
		if result.OK || result.Error == "" {
			t.Fatalf("invalid link should fail: %+v", result)
		}
	}
}
