# RefAlloc — Design

RefAlloc is a small, single-threaded memory allocator built directly on top of
the Windows virtual-memory API (`VirtualAlloc` / `VirtualFree`). It exposes two
functions:

```c
void* rnew(size_t bytes);
void  rfree(void* ptr);
```

Internally it routes every request to one of **three** allocation strategies,
each tuned for a different size range:

| Strategy            | Size range it serves        | Core idea                                  |
| ------------------- | --------------------------- | ------------------------------------------ |
| Fixed-size areas    | `1 … 512` bytes             | Segregated free lists ("size classes")     |
| Medium heap         | `513 … 64000` bytes         | 256-byte block runs with split + coalesce  |
| Large object area   | `> 64000` bytes             | One OS allocation per object               |

The rest of this document explains *why* the allocator is structured this way
and, in detail, *how each size range is actually carved up*.

---

## 1. Design goals

* **Fast in the common case.** Small allocations dominate real workloads, so the
  small path is `O(1)` for both allocation and free.
* **Low waste.** Internal fragmentation is bounded by choosing different
  strategies per size: cheap size classes for tiny objects, a finer block heap
  for mid-sized objects, and direct OS mapping for huge objects where
  subdivision buys nothing.
* **Few OS round-trips.** Virtual allocations are expensive, so RefAlloc carves
  many user objects out of each OS page and keeps a small reserve of empty pages
  ("hysteresis") instead of returning them immediately.
* **Self-describing memory.** `rfree` is given only a raw pointer, so every OS
  allocation must encode enough information to recover which strategy owns it.
* **Understandable.** It is a learning project; correctness and clarity are
  preferred over the aggressive optimizations used by jemalloc / mimalloc /
  tcmalloc.

---

## 2. The virtual-memory foundation

All memory ultimately comes from:

```c
VirtualAlloc(NULL, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
VirtualFree(p, 0, MEM_RELEASE);
```

The allocator's notion of a "page" is the **OS allocation granularity**
(`GetSystemInfo().dwAllocationGranularity`, typically **64 KB** on Windows), not
the 4 KB hardware page. This matters because `VirtualAlloc` always returns
addresses aligned to that granularity — a fact the free path relies on (see
§6).

Every OS allocation begins with a one-word **association tag**:

```c
enum AllocationKind { FixedSizeArea, MediumHeap, LargeObjectHeap };
```

It is always the first field of whatever header sits at the start of the
allocation (`FixedBlockPage`, `HeapPage`, or `LargeObject`). This is the key
that lets `rfree` dispatch correctly.

---

## 3. How a request is routed

`rnew(n)` → `do_alloc(n)` makes the size decision with two comparisons, checked
largest-first:

```c
if (n > 64000)            -> large object area      // mediumHeapMaxAllocSize
if (n > 512)              -> medium heap            // largest size class
else                      -> fixed-size area
```

* `512` is simply the block size of the largest fixed-size area.
* `64000` is `mediumHeapMaxAllocSize`. The code first computes the true
  "largest object that still fits in one medium page after its header and
  footer," then pins the limit to a round `64000`. Anything larger cannot be
  guaranteed to fit inside a single medium page, so it becomes a large object.

Worked examples:

| `rnew` size | Routed to     | Notes                                  |
| ----------- | ------------- | -------------------------------------- |
| `1`         | fixed `area8` | smallest class                         |
| `8`         | fixed `area8` |                                        |
| `9`         | fixed `area16`|                                        |
| `70`        | fixed `area128`|                                       |
| `257`       | fixed `area512`| no 384 class, so it rounds up to 512  |
| `513`       | medium heap   | just over the largest class            |
| `64000`     | medium heap   | last size that fits a single page      |
| `64001`     | large object  | one dedicated OS allocation            |
| `200000`    | large object  |                                        |

---

## 4. Small allocations — fixed-size areas (size classes)

### 4.1 The classes

Small objects are served from eight **areas**, each handing out blocks of one
fixed size:

```
8, 16, 32, 64, 128, 192, 256, 512  (bytes)
```

stored in ascending order in `specificAreas[]`.

The spacing is deliberate. Powers of two alone (`8,16,…,512`) would waste up to
~50% on the worst-fit object in each class, so an extra **192** class is wedged
between 128 and 256 to tighten the gap where mid-small structs cluster.

The floor is **8 bytes** because a free block must store a `next` pointer:

```c
struct MemoryBlock { MemoryBlock* next; };   // 8 bytes
static_assert(sizeof(MemoryBlock) <= 8, ...);
```

A free block reuses *its own payload space* to hold that pointer, so a live
allocation carries **zero** per-block metadata.

### 4.2 Selecting a class for a given size

`do_alloc` does a short linear scan and picks the **smallest class whose block
size is ≥ the request**:

```c
for (Area* a : specificAreas)
    if (n <= a->blockSize) { chosen = a; break; }
```

So the size→class mapping is "round the request up to the next class size":

| Request `n`         | Class    | Internal waste (worst case in range) |
| ------------------- | -------- | ------------------------------------ |
| `1 … 8`             | `area8`  | up to 7 B                            |
| `9 … 16`            | `area16` | up to 7 B                            |
| `17 … 32`           | `area32` | up to 15 B                           |
| `33 … 64`           | `area64` | up to 31 B                           |
| `65 … 128`          | `area128`| up to 63 B                           |
| `129 … 192`         | `area192`| up to 63 B                           |
| `193 … 256`         | `area256`| up to 63 B                           |
| `257 … 512`         | `area512`| up to 255 B                          |

This is the classic size-class tradeoff: allocation is `O(1)` and the free list
is trivial, at the cost of bounded internal fragmentation per object.

### 4.3 Page layout

Each area owns one or more `FixedBlockPage`s. A page is a single OS allocation
(`PAGE_SIZE`), laid out as a header followed by equal-sized blocks:

```
FixedBlockPage (PAGE_SIZE bytes)
+-----------+--------+--------+--------+-----+
|  header   | block0 | block1 | block2 | ... |   each block = area->blockSize
+-----------+--------+--------+--------+-----+
```

The header holds the association tag, a back-pointer to the owning area, total
and free block counts, the free-list head, list links, and the page's state.
The first block offset is rounded up past `sizeof(FixedBlockPage)`, so the
header effectively consumes the first block's worth of space.

At page creation, every block is threaded onto a singly-linked **free list**:

```
freeHead -> block -> block -> block -> NULL
```

Allocation = pop the head; free = push onto the head. Both `O(1)`.

### 4.4 Page state lists

To avoid scanning, each area keeps its pages on **three** doubly-linked lists by
occupancy:

```
Area
 ├─ freePages    : every block free
 ├─ partialPages : some blocks free, some used
 └─ fullPages    : no free blocks
```

### 4.5 Allocation flow (`alloc_fixed_size`)

1. **Partial page first.** Pop a block from the head partial page. If that
   empties the page's free list, move the page `partial → full`.
2. **Otherwise a free page.** If no partial page exists, take a block from a
   free page and move it `free → partial`.
3. **Otherwise grow.** If there are no free pages, `VirtualAlloc` a new page,
   initialize its free list, place it in `freePages`, then take a block from it.

Preferring partial pages keeps free pages genuinely empty so they can be
returned to the OS, and concentrates live objects on fewer pages.

### 4.6 Free flow (`dealloc_small_object`)

1. Push the block back onto its page's free list (`numFreeBlocks++`).
2. **If the page became entirely free** (`numFreeBlocks == numTotalBlocks`),
   unlink it from wherever it was and move it to `freePages`.
3. **Trim the reserve.** While `freePages` holds more than
   `NUM_FREE_PAGES_RESERVE` (= 1) pages, `VirtualFree` the extra ones.

Keeping one spare empty page per area is the hysteresis that prevents
alloc/free churn from repeatedly hitting the OS at a page boundary.

> **Design note — the full list is "sticky."** A page only leaves the `full`
> list when it becomes *completely* empty; freeing a single block from a full
> page does **not** promote it back to `partial`. The just-freed block therefore
> isn't reused until the whole page drains. This is a deliberate simplification
> that keeps the free path branch-light, at the cost of some unreusable slack on
> heavily-recycled full pages.

---

## 5. Medium heap

### 5.1 Why a separate strategy

Extending size classes upward would be wasteful: a 600-byte object in a
power-of-two world needs a 1024-byte block (~40% lost). For this range RefAlloc
instead uses a block heap with **splitting and coalescing**, trading a little
speed for much tighter packing.

### 5.2 Page layout — 256-byte blocks

A medium page is one OS allocation (`PAGE_SIZE`) viewed as an array of
fixed-size **blocks** of:

```c
MED_HEAP_BLOCK_SIZE = 256;
```

```
HeapPage
+-----------+------+------+------+------+-----+
|  header   | 256B | 256B | 256B | 256B | ... |
+-----------+------+------+------+------+-----+
```

256 bytes is the heap's quantum: allocations are measured and split in whole
blocks. It is chosen so a free-block descriptor plus its footer always fit
inside a single block (enforced by a `static_assert`).

### 5.3 Block sequences + boundary tags

Free and used regions are described by a **block sequence**, a contiguous run of
blocks. Representing "12 free blocks" as one sequence instead of twelve
individual blocks keeps metadata tiny.

Every sequence is wrapped in **boundary tags** — a header at the front and a
footer at the back:

```
+------------------+
|  BlockSequence   |  header: beginBlock, numBlocks, used, list links
+------------------+
|    user data     |
+------------------+
| BlockSeqFooter   |  footer: numBlocks (a copy)
+------------------+
```

The footer is what makes coalescing possible: given a sequence, the *previous*
neighbor's footer sits immediately before this sequence's header, and the footer
records the neighbor's length — so its header can be located in `O(1)` without
scanning. Helper math:

```c
footer = (char*)seq + seq->numBlocks * 256 - sizeof(footer);   // header -> footer
seq    = (char*)footer + sizeof(footer) - footer->numBlocks*256;// footer -> header
```

### 5.4 How a request becomes a block count

This is the heart of medium sizing. For a request of `n` bytes:

```c
totalRequiredBytes = allocHeaderSize + n + sizeof(BlockSequenceFooter);
numRequiredBlocks  = ceil(totalRequiredBytes / 256);
```

That is: every medium allocation pays for its **own header (~48 B)** and a
**footer (8 B)**, and the total is rounded **up to a whole number of 256-byte
blocks**. The user pointer is returned just past the header.

| Request `n` | `header+n+footer` | Blocks (`×256`) | Bytes consumed |
| ----------- | ----------------- | --------------- | -------------- |
| `513`       | ~569              | 3               | 768            |
| `600`       | ~656              | 3               | 768            |
| `2048`      | ~2104             | 9               | 2304           |
| `4000`      | ~4056             | 16              | 4096           |

So medium internal fragmentation is at most one block (255 B) of rounding plus
the ~56 B of header/footer — a small fraction once objects are this large.

### 5.5 Choosing a page and splitting

Each page caches `page->blockSeq`, a pointer to (heuristically) its **largest
free sequence**, kept at the head of that page's free list. Allocation:

1. Compute `numRequiredBlocks`.
2. If the **head partial page's** largest sequence has enough blocks, use that
   page. Otherwise, if a free page exists, use it. Otherwise `VirtualAlloc` a new
   page (it starts as one big free sequence) and use that.
3. **Split** the chosen sequence. The request is carved off the front; the
   remainder becomes a new free sequence placed back as the page's largest:

```
  before:  [ ───────── 20 free blocks ───────── ]
  request 5 blocks:
  after:   [ 5 used ][ ─────── 15 free ─────── ]
                       ^ new free remainder, footer rewritten
```

   If the fit is exact, no remainder is created and the head advances to the
   next free sequence in the page.
4. If the page was previously free, move it `free → partial`.

### 5.6 The largest-sequence ordering heuristic

The partial-page list is kept **approximately** sorted so that the **head page
holds the largest free sequence of any partial page**. The invariant lets step 2
above check only the head: if even the head can't satisfy the request, the
allocator assumes no partial page can and grabs a fresh page.

The ordering is maintained cheaply by `heuristic_push_partial_page_back()`,
which performs a **single adjacent swap** of the first two pages whenever the
first's largest sequence is smaller than the second's. This is one step of a
bubble sort, not a full sort, so the ordering is a fast approximation rather than
an exact ranking. The consequence: occasionally a fresh page is allocated even
though some deeper partial page could have served the request — a deliberate
speed-for-simplicity trade.

### 5.7 Free flow and coalescing (`dealloc_heap`)

1. Recover the `BlockSequence` header from the user pointer.
2. **Coalesce right.** If this isn't the last sequence in the page and the right
   neighbor is free, absorb it (extend `numBlocks`, rewrite the footer, unlink
   the neighbor).
3. **Coalesce left.** If this isn't the first sequence and the left neighbor is
   free (found via its footer), let the left neighbor absorb this block.
4. Insert the resulting free sequence back into the page's list — as the new head
   if it's larger than the current head, otherwise just behind it.
5. Re-link the page in the heap's partial list and run the ordering heuristic.
6. **If the page is now entirely free** (`largest == totalNumBlocks`), move it to
   `freePages` and trim the reserve (`NUM_FREE_PAGES_RESERVE`) back to the OS.

```
  before free of the middle run:
  [ 10 free ][ 5 used ][ 7 free ]
  after:
  [ ───────── 22 free ───────── ]
```

Coalescing is what keeps **external** fragmentation in check — adjacent holes
merge back into large runs instead of accumulating as unusable gaps.

---

## 6. Large object area

### 6.1 Sizing — direct 1:1 OS mapping

For `n > 64000`, subdivision is pointless: the object already fills most of one
or more OS pages, so fragmentation within it is irrelevant. Each large object is
its **own** `VirtualAlloc`:

```c
totalSizeNeeded = n + loAlignedHeaderSize;   // header ~48 B, 16-aligned
LargeObject* lo = VirtualAlloc(totalSizeNeeded ...);
```

There are **no size classes and no rounding** beyond what the OS itself does
when it reserves/commits the region. The layout is just a header followed by the
user region:

```
+--------------+
| LargeObject  |  tag, dataHead, objectSize, list links
+--------------+
|  user memory |  <- returned pointer
+--------------+
```

### 6.2 Tracking and free

Live large objects are threaded onto a doubly-linked list (`LargeObjectArea`),
purely for bookkeeping/debugging — it is not consulted on the hot path. Freeing
is:

1. Recover the header (`ptr − loAlignedHeaderSize`).
2. Unlink from the list.
3. `VirtualFree(..., MEM_RELEASE)`.

No pooling, no coalescing — the OS handles reuse of the address space.

---

## 7. Freeing without size information

`rfree(ptr)` receives only a raw pointer, yet must know which strategy to call.
RefAlloc solves this with the alignment guarantee from §2: every OS allocation
starts on a `PAGE_SIZE` (allocation-granularity) boundary, and the
`AllocationKind` tag is the first field of the header there. So:

```c
header = (ptr / PAGE_SIZE) * PAGE_SIZE;   // round the pointer down to its page base
switch (*(AllocationKind*)header) {
    case FixedSizeArea:    dealloc_small_object(...); break;
    case MediumHeap:       dealloc_heap(...);         break;
    case LargeObjectHeap:  dealloc_large_object(...); break;
}
```

This works for all three:

* **Fixed / medium pages** — any user pointer inside the page rounds down to the
  page header (no single allocation crosses a page boundary).
* **Large objects** — the header is only ~48 B in, so the returned pointer rounds
  straight back to the allocation base.

The contract is that the pointer passed to `rfree` is exactly the one `rnew`
returned; interior pointers would break the rounding.

---

## 8. Alignment

`rnew` guarantees **16-byte** alignment (`MAX_ALIGNMENT`), the width of the
widest primitive the allocator promises to support.

* **Medium / large** — all headers are rounded up to a 16-byte boundary and the
  block quantum (256) is a multiple of 16, so payloads land on 16.
* **Fixed-size** — page bases are 64 KB aligned and block sizes are multiples of
  16 for every class **except `area8`**, whose blocks are only 8-aligned. This
  is harmless: any object that genuinely needs 16-byte alignment is larger than
  8 bytes and is therefore routed to `area16` or above.

---

## 9. Complexity

| Operation         | Complexity     |
| ----------------- | -------------- |
| Small allocation  | `O(1)`         |
| Small free        | `O(1)`         |
| Medium allocation | `O(1)` average |
| Medium free       | `O(1)`         |
| Large allocation  | `O(OS)`        |
| Large free        | `O(OS)`        |

---

## 10. Tradeoffs and limitations

* **Not thread-safe.** No locks, no per-thread caches, no lock-free structures.
* **Approximate medium ordering.** The single-swap heuristic can occasionally
  allocate a new page when a deeper partial page would have fit.
* **Sticky full pages (fixed-size).** A full page's freed blocks aren't reused
  until the page drains completely (§4.6).
* **Size-class internal fragmentation.** Up to nearly 50% for worst-fit objects
  in the small range — the price of `O(1)` simplicity.
* **Less optimized than production allocators** (jemalloc, mimalloc, tcmalloc),
  by design — clarity first.

---

## 11. What this design demonstrates

Although compact, RefAlloc exercises most of the foundational techniques real
allocators use: segregated free lists and size classes, page/arena management,
boundary tags, block splitting and coalescing, free-page hysteresis, a
self-describing tag for type-erased frees, and direct interaction with the OS
virtual-memory layer — all small enough to be read and understood as a single
system.