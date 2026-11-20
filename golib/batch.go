package mewnet

import (
	"context"
	"sync"
)

// LinkTest is the result of one link check.
type LinkTest struct {
	Link    string
	OK      bool
	DelayMS uint16
	Error   string
	Name    string
	Type    string
	Server  string
	Port    int
}

func (c *Client) testLinksBatch(ctx context.Context, links []string, opts DelayOptions, concurrency int, stopOnSuccess bool) []LinkTest {
	results := make([]LinkTest, len(links))
	for i, link := range links {
		results[i].Link = link
	}
	if c == nil || len(links) == 0 {
		return results
	}
	workers := concurrency
	if workers <= 0 {
		workers = 1
	}
	if workers > len(links) {
		workers = len(links)
	}
	if workers > 256 {
		workers = 256
	}

	runCtx := ctx
	cancel := func() {}
	if stopOnSuccess {
		runCtx, cancel = context.WithCancel(ctx)
	}
	defer cancel()

	jobs := make(chan int)
	var wg sync.WaitGroup
	var found sync.Once
	for w := 0; w < workers; w++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for index := range jobs {
				if stopOnSuccess {
					select {
					case <-runCtx.Done():
						results[index].Error = runCtx.Err().Error()
						continue
					default:
					}
				}
				result, err := c.TestLinkContext(runCtx, links[index], opts)
				if err != nil {
					results[index].Error = err.Error()
					continue
				}
				results[index].OK = true
				results[index].DelayMS = result.Delay
				results[index].Name = result.Name
				results[index].Type = result.Type
				results[index].Server = result.Server
				results[index].Port = result.Port
				if stopOnSuccess {
					found.Do(cancel)
				}
			}
		}()
	}
	for i := range links {
		jobs <- i
	}
	close(jobs)
	wg.Wait()
	return results
}
