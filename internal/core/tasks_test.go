package core

import (
	"context"
	"testing"
	"time"
)

func TestCancelLinksWaitsForCallback(t *testing.T) {
	store := newTaskStore()
	callbackDone := make(chan struct{})

	id := store.start(TaskKindLinkPing, []uint64{7}, func(ctx context.Context) (any, error) {
		<-ctx.Done()
		return nil, ctx.Err()
	}, func(TaskSnapshot) {
		close(callbackDone)
	})

	store.cancelLinks(7)

	select {
	case <-callbackDone:
	default:
		t.Fatal("cancelLinks returned before the callback finished")
	}

	snapshot, ok := store.poll(id)
	if !ok {
		t.Fatal("task disappeared")
	}
	if snapshot.Status != TaskCancelled {
		t.Fatalf("status = %v, want cancelled", snapshot.Status)
	}
}

func TestCancelLinksIgnoresOtherLinks(t *testing.T) {
	store := newTaskStore()
	release := make(chan struct{})
	callbackDone := make(chan struct{})

	id := store.start(TaskKindLinkPing, []uint64{2}, func(context.Context) (any, error) {
		<-release
		return nil, nil
	}, func(TaskSnapshot) {
		close(callbackDone)
	})

	store.cancelLinks(1)

	select {
	case <-callbackDone:
		t.Fatal("task for another link was cancelled")
	default:
	}

	snapshot, ok := store.poll(id)
	if !ok {
		t.Fatal("task disappeared")
	}
	if snapshot.Status != TaskRunning {
		t.Fatalf("status = %v, want running", snapshot.Status)
	}

	close(release)
	select {
	case <-callbackDone:
	case <-time.After(2 * time.Second):
		t.Fatal("callback did not run")
	}
}

func TestFreeTaskWaitsForCallback(t *testing.T) {
	store := newTaskStore()
	release := make(chan struct{})
	callbackStarted := make(chan struct{})

	id := store.start(TaskKindDelay, nil, func(context.Context) (any, error) {
		return nil, nil
	}, func(TaskSnapshot) {
		close(callbackStarted)
		<-release
	})

	select {
	case <-callbackStarted:
	case <-time.After(2 * time.Second):
		t.Fatal("callback did not start")
	}

	freed := make(chan struct{})
	go func() {
		store.free(id)
		close(freed)
	}()

	select {
	case <-freed:
		t.Fatal("free returned while the callback was still running")
	case <-time.After(100 * time.Millisecond):
	}

	close(release)
	select {
	case <-freed:
	case <-time.After(2 * time.Second):
		t.Fatal("free did not return after the callback finished")
	}
}
