package core

import (
	"context"
	"encoding/json"
	"sync"
	"time"
)

const (
	taskRunning   = "running"
	taskDone      = "done"
	taskCancelled = "cancelled"

	taskTTL = 10 * time.Minute
)

type task struct {
	id     int64
	cancel context.CancelFunc
	done   chan struct{}

	mu       sync.Mutex
	status   string
	result   []byte
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

func (s *taskStore) start(run func(context.Context) (any, error), cb func(int64, []byte)) int64 {
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
		id:     id,
		cancel: cancel,
		done:   make(chan struct{}),
		status: taskRunning,
	}
	s.tasks[id] = t
	s.wg.Add(1)
	s.mu.Unlock()

	go func() {
		defer s.wg.Done()
		defer cancel()

		value, err := run(ctx)
		status := taskDone
		if ctx.Err() == context.Canceled {
			status = taskCancelled
		}

		var payload map[string]any
		if err != nil {
			payload = map[string]any{"ok": false, "error": err.Error()}
			if status == taskCancelled {
				payload["cancelled"] = true
			}
		} else {
			payload = map[string]any{"ok": true}
			switch v := value.(type) {
			case map[string]any:
				for k, item := range v {
					payload[k] = item
				}
			default:
				payload["result"] = v
			}
		}
		encoded, marshalErr := json.Marshal(payload)
		if marshalErr != nil {
			encoded = []byte(`{"ok":false,"error":"encode task result"}`)
		}

		t.mu.Lock()
		t.status = status
		t.result = encoded
		t.finished = time.Now()
		t.mu.Unlock()
		close(t.done)

		if cb != nil {
			cb(id, encoded)
		}
	}()

	return id
}

func (s *taskStore) poll(id int64) ([]byte, bool) {
	s.mu.Lock()
	s.sweepLocked(time.Now())
	t := s.tasks[id]
	s.mu.Unlock()
	if t == nil {
		return nil, false
	}

	t.mu.Lock()
	status := t.status
	result := append([]byte(nil), t.result...)
	t.mu.Unlock()

	if status == taskRunning {
		out, _ := json.Marshal(map[string]any{
			"ok":      true,
			"task_id": id,
			"status":  taskRunning,
		})
		return out, true
	}

	out, _ := json.Marshal(map[string]any{
		"ok":      true,
		"task_id": id,
		"status":  status,
		"result":  json.RawMessage(result),
	})
	return out, true
}

// PollTask returns the JSON state of an async task.
func (c *Core) PollTask(id int64) (string, bool) {
	data, ok := c.tasks.poll(id)
	if !ok {
		return "", false
	}
	return string(data), true
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

func (s *taskStore) sweepLocked(now time.Time) {
	for id, t := range s.tasks {
		t.mu.Lock()
		finished := t.finished
		status := t.status
		t.mu.Unlock()
		if status != taskRunning && !finished.IsZero() && now.Sub(finished) > taskTTL {
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
