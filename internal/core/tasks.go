package core

import (
	"context"
	"sync"
	"time"
)

// TaskKind identifies the operation that produced a task result.
type TaskKind int

const (
	TaskKindUnknown TaskKind = iota
	TaskKindDelay
	TaskKindTestLink
	TaskKindLinkPing
	TaskKindLinkSpeed
	TaskKindLinkExitIP
	TaskKindPingMany
)

// TaskStatus is the lifecycle state of an asynchronous task.
type TaskStatus int

const (
	TaskRunning TaskStatus = iota
	TaskDone
	TaskCancelled
)

// TaskSnapshot is an immutable view of an asynchronous task.
type TaskSnapshot struct {
	ID     int64
	Kind   TaskKind
	Status TaskStatus
	Value  any
	Err    error
}

const taskTTL = 10 * time.Minute

type task struct {
	id      int64
	kind    TaskKind
	linkIDs []uint64
	cancel  context.CancelFunc
	done    chan struct{}

	mu       sync.Mutex
	status   TaskStatus
	value    any
	err      error
	finished time.Time
}

type taskStore struct {
	mu     sync.Mutex
	tasks  map[int64]*task
	next   int64
	wg     sync.WaitGroup
	closed bool
}

func newTaskStore() *taskStore {
	return &taskStore{tasks: make(map[int64]*task)}
}

func (s *taskStore) start(kind TaskKind, linkIDs []uint64, run func(context.Context) (any, error), cb func(TaskSnapshot)) int64 {
	s.mu.Lock()
	if s.closed {
		s.mu.Unlock()
		return -1
	}
	s.sweepLocked(time.Now())
	s.next++
	id := s.next
	ctx, cancel := context.WithCancel(context.Background())
	t := &task{
		id:      id,
		kind:    kind,
		linkIDs: append([]uint64(nil), linkIDs...),
		cancel:  cancel,
		done:    make(chan struct{}),
		status:  TaskRunning,
	}
	s.tasks[id] = t
	s.wg.Add(1)
	s.mu.Unlock()

	go func() {
		defer s.wg.Done()
		defer cancel()

		value, err := run(ctx)
		status := TaskDone
		if ctx.Err() == context.Canceled {
			status = TaskCancelled
		}

		t.mu.Lock()
		t.status = status
		t.value = value
		t.err = err
		t.finished = time.Now()
		snapshot := TaskSnapshot{ID: id, Kind: kind, Status: status, Value: value, Err: err}
		t.mu.Unlock()

		// The callback runs before done is closed so that FreeTask and
		// cancelLinks wait for the callback to finish as well.
		if cb != nil {
			cb(snapshot)
		}
		close(t.done)
	}()

	return id
}

func (s *taskStore) poll(id int64) (TaskSnapshot, bool) {
	s.mu.Lock()
	s.sweepLocked(time.Now())
	t := s.tasks[id]
	s.mu.Unlock()
	if t == nil {
		return TaskSnapshot{}, false
	}

	t.mu.Lock()
	snapshot := TaskSnapshot{
		ID:     id,
		Kind:   t.kind,
		Status: t.status,
		Value:  t.value,
		Err:    t.err,
	}
	t.mu.Unlock()
	return snapshot, true
}

// PollTask returns the current state of an async task.
func (c *Core) PollTask(id int64) (TaskSnapshot, bool) {
	return c.tasks.poll(id)
}

// CancelTask cancels a running async task.
func (c *Core) CancelTask(id int64) bool {
	return c.tasks.cancel(id)
}

// FreeTask cancels (if still running) and forgets an async task.
func (c *Core) FreeTask(id int64) bool {
	return c.tasks.free(id)
}

func (s *taskStore) cancel(id int64) bool {
	s.mu.Lock()
	t := s.tasks[id]
	s.mu.Unlock()
	if t == nil {
		return false
	}
	t.cancel()
	return true
}

func (s *taskStore) free(id int64) bool {
	s.mu.Lock()
	t := s.tasks[id]
	s.mu.Unlock()
	if t == nil {
		return false
	}
	t.cancel()
	<-t.done

	s.mu.Lock()
	delete(s.tasks, id)
	s.mu.Unlock()
	return true
}

// cancelLinks cancels every task that references at least one of the given
// links and waits for those tasks to finish. It must not be called from a task
// callback of the same link.
func (s *taskStore) cancelLinks(ids ...uint64) {
	if len(ids) == 0 {
		return
	}
	wanted := make(map[uint64]bool, len(ids))
	for _, id := range ids {
		wanted[id] = true
	}

	s.mu.Lock()
	var tasks []*task
	for _, t := range s.tasks {
		for _, linkID := range t.linkIDs {
			if wanted[linkID] {
				tasks = append(tasks, t)
				break
			}
		}
	}
	s.mu.Unlock()

	for _, t := range tasks {
		t.cancel()
	}
	for _, t := range tasks {
		<-t.done
	}
}

func (s *taskStore) sweepLocked(now time.Time) {
	for id, t := range s.tasks {
		t.mu.Lock()
		finished := t.finished
		status := t.status
		t.mu.Unlock()
		if status != TaskRunning && !finished.IsZero() && now.Sub(finished) > taskTTL {
			delete(s.tasks, id)
		}
	}
}

func (s *taskStore) shutdown() {
	s.mu.Lock()
	if s.closed {
		s.mu.Unlock()
		return
	}
	s.closed = true
	tasks := make([]*task, 0, len(s.tasks))
	for _, t := range s.tasks {
		tasks = append(tasks, t)
	}
	s.mu.Unlock()

	for _, t := range tasks {
		t.cancel()
	}
	s.wg.Wait()

	s.mu.Lock()
	s.tasks = make(map[int64]*task)
	s.mu.Unlock()
}
