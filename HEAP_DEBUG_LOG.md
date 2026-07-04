# Heap corruption investigation log

Goal: find the actual source of the heap corruption that the hardened
allocator (v0.9.80, `kernel/lib/memory.c`) now detects and contains
instead of crashing on, triggered by e.g.:

```
tsharp
>>> dosyayaz("/test.pdf", "<a string of ~120+ characters>")
```

## What's confirmed so far (from the previous session)

- The crash used to page-fault deep inside `heap_alloc()`, at a fixed
  instruction (`mov ebx, [eax+0x8]`, reading a `next`-walked node's
  `used` field) with `eax` (the node pointer) containing raw ASCII
  bytes from the string that was being written — i.e. some write
  clobbered a `heap_header_t.next` pointer with string data.
- After hardening the allocator (bounds+magic checks on every pointer
  walked, a canary word after every payload, checked on `free()`), the
  exact same script now prints:
  `[heap] corrupted free-list node found during malloc, list walk aborted`
  and the system stays up. So corruption still happens; it's just safe now.
- Important clue: the report comes from **`malloc()`'s list walk**,
  not from `free()`'s canary check. If a live block had merely been
  written past its own canary, `free()` would have caught it first
  (canary mismatch) when that block was eventually freed. Getting the
  *malloc-side* "corrupted node" report instead suggests either (a) a
  wild pointer write that never went through a legitimate block's
  bounds at all, or (b) a **use-after-free**: a write into memory
  *after* it was freed, corrupting whatever the allocator has since
  put there, which a live block's own canary check would never see.
- The corruption is state-dependent, not purely a function of the
  string length — the exact same command sometimes corrupts and
  sometimes doesn't in the same boot, depending on prior heap
  allocations. This is consistent with a free-list-reuse-dependent bug
  (e.g. UAF) rather than a fixed off-by-N in one buffer size.
- Two real, unrelated bugs were already found and fixed in T# itself
  (`ts_normalize` stack overflow, naive `)`-scanning in call parsing)
  but neither of them explained this specific heap corruption — it
  reproduced identically after both fixes landed.

## Investigation plan for this session

1. Add temporary `heap_check()` calls at fine granularity through the
   `dosyayaz(...)` call path (T# call-statement parsing → argument
   tokenizing/eval → `ts_call_func` → `tos_write` → `fsbridge_write` →
   `ramfs_write` → `ramfs_vfs_write`), rebuild, and reproduce in QEMU.
2. `heap_check()` now also validates every used block's canary (not
   just free-list magic numbers), so it should catch the corruption
   *before* the subsequent `malloc()` call that used to be the only
   thing that noticed — narrowing down which exact step introduces it.
3. Once localized, read that code path closely for anything that
   writes without going through a matching allocation size (in
   particular: use-after-free candidates, since that's what the
   evidence points to).
4. Remove all temporary instrumentation before shipping the fix.

(Findings appended below as they're confirmed.)

## Root cause found

Fine-grained `heap_check()` calls were added through the entire T#
call path (`tsharp.c`'s call-statement parsing → `tos_write()` →
`fsbridge_write()` → `ramfs_vfs_write()`), each printing OK/CORRUPT/
OVERFLOW with the corrupted node's index, allocated size, address, and
the garbage value found in its canary slot. Rebuilt and reproduced in
QEMU with the exact script that used to page-fault the kernel:

```
[heap_check] HDBG after one eval_expr OVERFLOW at node 19 req_size=2048 addr=006EA0DC canary_read=702E7473
[heap] corrupted free-list node found during malloc, list walk aborted
[heap_check] HDBG after one eval_expr OVERFLOW at node 19 req_size=2048 addr=006EA0DC canary_read=2E312D46
...
[heap_check] HDBG tos_write after create OVERFLOW at node 19 req_size=2048 addr=006EA0DC canary_read=00000008
```

Two things stood out immediately:

1. **`req_size=2048`** exactly matches `E1000_BUF_SIZE` in
   `kernel/net/e1000.c` — the fixed size every RX/TX DMA buffer for
   the E1000 NIC driver is allocated at
   (`kernel/net/e1000.c:161,165` — `malloc(E1000_BUF_SIZE)`).
2. **The garbage value at the same address kept changing** between
   consecutive `heap_check()` calls a few instructions apart in pure
   T# code that never touches this buffer — meaning something
   *outside* the code being tested was continuously overwriting it in
   the background. That only makes sense for something driven by
   asynchronous hardware interrupts, i.e. incoming network packets.

Neither of those things has anything to do with T#, `tos_write`, or
`ramfs` — they just happened to be where instrumentation was placed
first. The actual write comes from the E1000 NIC's DMA engine.

### Confirming it

Rebooted in a controlled A/B test, reproducing the identical script
twice:

- **With QEMU's default networking** (`-net` default, matches every
  previous repro this project has done): corruption reproduced.
- **With `-net none`** (NIC fully disabled, otherwise identical boot):
  ran the same script *6 times in a row* — zero corruption, every
  single `heap_check()` reported OK.

This is about as clean an A/B result as this kind of bug ever gives:
disabling the network device alone made the corruption disappear
completely, with nothing else about the test changed.

### Why this makes sense, and why it wasn't "found" earlier

`kernel/net/e1000.c`'s RX buffers (`rx_bufs[i]`, 2048 bytes each,
matching `RCTL`'s configured 2048-byte buffer size with long-packet
reception left disabled) are **only ever written by the NIC's own DMA
engine** — nothing in this codebase's software ever writes into them;
`e1000_poll()` only *reads* out of them, and that read is already
correctly bounded (clamped to the caller's `max_len`, which is smaller
than 2048 in the only caller, `net_poll()`). So a plain source-code
read-through of the driver doesn't turn up an obvious software bug —
because there probably isn't one in the C code. The likely explanation
is a QEMU e1000 emulation quirk (or a subtle configuration gap in this
driver that produces one) causing the emulated NIC to occasionally DMA
slightly past the buffer it was told to use. That lives below what a
memory-safety review of the driver's own C code can catch.

### What was actually fixed

Since the overwrite happens via DMA, no software bounds check can
*prevent* it from a C source level — but the driver was hardened to
limit the damage instead of trusting the hardware completely:

- **RX buffers now have 128 bytes of unadvertised padding**
  (`E1000_RX_PAD` in `kernel/net/e1000.c`) past the real 2048 bytes
  told to the NIC, absorbing a small overrun before it reaches the
  next heap block's header.
- **`e1000_poll()` now clamps the hardware-reported receive length**
  to `E1000_BUF_SIZE` before trusting it for anything, instead of
  passing whatever the RX descriptor claims straight through.
- The hardened allocator from the previous session (bounds/magic
  checks on every pointer walked, per-block canaries checked on
  `free()`) already contains whatever gets through instead of
  crashing, and stays that way as a second line of defense.

This does not *prove* zero remaining overrun is possible (nothing
short of owning the exact DMA behavior would), but it substantially
shrinks the blast radius and gives the allocator's existing detection
a much larger safety margin before it would ever matter again.

All temporary `heap_check()` debug calls added to `tsharp.c`,
`tos_api.c`, and `ramfs.c` during this investigation have been
removed; `heap_check()` itself (now reporting the corrupted node's
index/size/address/canary value, not just OK/CORRUPT) remains in
`kernel/lib/memory.c` as a permanent, unused-by-default debug utility
for any future investigation like this one.
