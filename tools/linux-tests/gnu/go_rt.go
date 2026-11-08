package main

import (
	"crypto/sha256"
	"fmt"
	"os"
	"runtime"
	"sort"
	"strings"
	"sync"
	"time"
)

func main() {
	fmt.Println("go runtime:", runtime.Version(), runtime.GOOS, runtime.GOARCH, "cpus", runtime.NumCPU())
	var wg sync.WaitGroup
	var mu sync.Mutex
	sum := 0
	for i := 1; i <= 8; i++ {
		wg.Add(1)
		go func(n int) {
			defer wg.Done()
			s := 0
			for k := 0; k < 200000; k++ {
				s += k % n
			}
			mu.Lock()
			sum += s
			mu.Unlock()
		}(i)
	}
	wg.Wait()
	fmt.Println("goroutines sum", sum)
	ch := make(chan int)
	go func() { time.Sleep(20 * time.Millisecond); ch <- 42 }()
	select {
	case v := <-ch:
		fmt.Println("chan", v)
	case <-time.After(2 * time.Second):
		fmt.Println("TIMEOUT")
	}
	m := map[string]int{"b": 2, "a": 1, "c": 3}
	k := []string{}
	for x := range m {
		k = append(k, x)
	}
	sort.Strings(k)
	fmt.Println(strings.Join(k, ","))
	fmt.Printf("%x\n", sha256.Sum256([]byte("tOS")))
	os.WriteFile("/tmp/go_rt.txt", []byte("ok"), 0644)
	b, _ := os.ReadFile("/tmp/go_rt.txt")
	fmt.Println("file", string(b))
	buf := make([]byte, 64<<20)
	buf[len(buf)-1] = 1
	runtime.GC()
	fmt.Println("done")
}
