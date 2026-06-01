\# DESIGN.md



\# RefAlloc - Memory Allocator Design



\## Overview



RefAlloc is a custom memory allocator built from scratch on top of the Windows virtual memory API (`VirtualAlloc` / `VirtualFree`).



The allocator is divided into three allocation strategies:



1\. \*\*Fixed-Size Areas\*\* (small allocations)

2\. \*\*Medium Heap\*\* (medium-sized allocations)

3\. \*\*Large Object Area\*\* (large allocations)



Each strategy is optimized for a different allocation size range.



The primary goals of the allocator are:



\* Fast allocation and deallocation

\* Minimal fragmentation for small objects

\* Efficient reuse of virtual memory

\* Simple and understandable implementation

\* Full ownership of the allocator design without external dependencies



\---



\# High Level Architecture



```

&#x20;                   rnew(size)



&#x20;                        │

&#x20;                        ▼



&#x20;            ┌─────────────────────┐

&#x20;            │   Size <= 512 B ?   │

&#x20;            └─────────┬───────────┘

&#x20;                      │ Yes

&#x20;                      ▼



&#x20;               Fixed Size Areas



&#x20;                      │ No

&#x20;                      ▼



&#x20;      ┌──────────────────────────────┐

&#x20;      │ Fits inside Medium Heap Page │

&#x20;      └──────────────┬───────────────┘

&#x20;                     │ Yes

&#x20;                     ▼



&#x20;                 Medium Heap



&#x20;                     │ No

&#x20;                     ▼



&#x20;              Large Object Area

```



\---



\# Virtual Memory Model



The allocator ultimately obtains memory from Windows through:



```cpp

VirtualAlloc(...)

VirtualFree(...)

```



The operating system allocation granularity is used as the allocator page size.



Each OS allocation begins with metadata that identifies what kind of allocation it belongs to:



```cpp

enum AllocationKind

{

&#x20;   FixedSizeArea,

&#x20;   MediumHeap,

&#x20;   LargeObjectHeap

};

```



This allows `rfree()` to determine how a pointer should be released.



\---



\# Small Allocations: Fixed-Size Areas



\## Motivation



Small allocations are extremely common.



Allocating a separate virtual allocation for every small object would be wasteful and slow.



Instead, RefAlloc uses \*\*size classes\*\*.



\---



\## Size Classes



The allocator contains the following fixed-size areas:



| Area    | Block Size |

| ------- | ---------- |

| Area8   | 8          |

| Area16  | 16         |

| Area32  | 32         |

| Area64  | 64         |

| Area128 | 128        |

| Area192 | 192        |

| Area256 | 256        |

| Area512 | 512        |



When an allocation request arrives:



```cpp

rnew(70)

```



the allocator chooses the smallest area capable of holding it.



Example:



```cpp

70 bytes -> Area128

```



\---



\## FixedBlockPage



Each area owns pages of memory:



```cpp

FixedBlockPage

```



Each page is divided into equal-sized blocks.



Example:



```

64 KB page



+-----+-----+-----+-----+-----+

| 64  | 64  | 64  | 64  | ... |

+-----+-----+-----+-----+-----+

```



\---



\## Free Block List



Every free block stores:



```cpp

struct MemoryBlock

{

&#x20;   MemoryBlock\* next;

};

```



The free blocks form a singly linked list.



Allocation becomes:



```text

pop head

```



Deallocation becomes:



```text

push head

```



Both operations are O(1).



\---



\## Page States



Pages are grouped into three lists:



\### Free



All blocks are available.



\### Partial



Some blocks are allocated.



\### Full



No free blocks remain.



```

Area

&#x20;├─ Free Pages

&#x20;├─ Partial Pages

&#x20;└─ Full Pages

```



This prevents scanning pages during allocation.



\---



\## Allocation Flow



Allocation follows:



1\. Try a partial page.

2\. Otherwise use a free page.

3\. Otherwise allocate a new page from the OS.



Because a page always exposes its free list head:



```text

Allocation Complexity: O(1)

```



\---



\## Deallocation Flow



When a block is returned:



1\. Push block into free list.

2\. Update free block count.

3\. Move page between state lists if necessary.



If an entire page becomes free:



```text

Partial → Free

Full → Free

```



The allocator maintains a small reserve of free pages and returns excess pages back to the OS.



\---



\# Medium Heap



\## Motivation



Using size classes for larger objects causes excessive internal fragmentation.



Example:



```cpp

600 bytes

```



would require a 1024-byte block if only power-of-two size classes existed.



Instead, RefAlloc uses a medium heap.



\---



\## Medium Heap Pages



Each medium heap page is an OS allocation.



A page is subdivided into:



```cpp

MED\_HEAP\_BLOCK\_SIZE = 256 bytes

```



blocks.



Example:



```

Page



+----+----+----+----+----+

|256 |256 |256 |256 |256 |

+----+----+----+----+----+

```



Allocations consume one or more contiguous blocks.



\---



\## Block Sequences



Free space is represented by:



```cpp

BlockSequence

```



A BlockSequence describes a contiguous run of blocks.



Example:



```

\[ 12 free blocks ]

```



instead of:



```

\[1]\[1]\[1]\[1]\[1]\[1]\[1]\[1]\[1]\[1]\[1]\[1]

```



This dramatically reduces metadata overhead.



\---



\## Boundary Tags



Every free sequence contains:



\### Header



```cpp

BlockSequence

```



\### Footer



```cpp

BlockSequenceFooter

```



The footer stores:



```cpp

numBlocks

```



This allows immediate discovery of the neighboring block sequence during deallocation.



```

+---------+

| Header  |

+---------+



&#x20;user data



+---------+

| Footer  |

+---------+

```



\---



\## Splitting



Suppose a free sequence contains:



```

20 blocks

```



and an allocation requests:



```

5 blocks

```



The allocator splits:



```

20

&#x20;↓



\[5 allocated] \[15 free]

```



Only the remaining free sequence stays in the free list.



\---



\## Coalescing



When freeing:



1\. Check neighbor on the right.

2\. Check neighbor on the left.

3\. Merge adjacent free sequences.



Example:



```

Before:



\[10 free]

\[5 used]

\[7 free]



After free:



\[22 free]

```



This reduces external fragmentation.



\---



\## Largest Free Sequence Heuristic



Each page tracks:



```cpp

page->blockSeq

```



which points to the largest free sequence currently known in that page.



Pages are maintained so that the head of the partial page list tends to contain one of the largest free sequences.



Allocation therefore usually succeeds without scanning many pages.



\---



\## Medium Heap Allocation



Allocation process:



1\. Determine required number of blocks.

2\. Use largest available sequence.

3\. Split if necessary.

4\. Return user pointer.



\---



\## Medium Heap Deallocation



Deallocation process:



1\. Recover block header.

2\. Merge right neighbor if possible.

3\. Merge left neighbor if possible.

4\. Reinsert merged block into page.

5\. Move page between free/partial lists if needed.



If the page becomes completely free:



```text

Partial → Free

```



Excess free pages are returned to the OS.



\---



\# Large Object Area



\## Motivation



Very large allocations do not benefit from subdivision.



If an allocation consumes most of an entire page anyway, fragmentation becomes irrelevant.



\---



\## Allocation



Objects larger than the medium heap limit are allocated directly through:



```cpp

VirtualAlloc

```



Each allocation receives:



```cpp

LargeObject

```



metadata at the beginning.



```

+--------------+

| LargeObject  |

+--------------+

| User Memory  |

+--------------+

```



\---



\## Tracking



Large objects are maintained in a doubly-linked list:



```cpp

LargeObjectArea

```



This list exists primarily for bookkeeping and debugging.



\---



\## Deallocation



Freeing a large object:



1\. Recover header.

2\. Remove from linked list.

3\. Call:



```cpp

VirtualFree(...)

```



No coalescing or pooling is required.



\---



\# Determining Allocation Type During Free



A major design challenge is:



```cpp

rfree(ptr)

```



must determine where the pointer originated.



RefAlloc solves this by aligning the pointer downward to the beginning of its OS allocation:



```cpp

userPtr /= PAGE\_SIZE;

userPtr \*= PAGE\_SIZE;

```



The allocator then reads:



```cpp

AllocationKind

```



stored at the beginning of the allocation.



This immediately identifies whether the pointer belongs to:



\* Fixed Size Area

\* Medium Heap

\* Large Object Area



and dispatches to the appropriate free routine.



\---



\# Complexity Summary



| Operation         | Complexity   |

| ----------------- | ------------ |

| Small Allocation  | O(1)         |

| Small Free        | O(1)         |

| Medium Allocation | O(1) average |

| Medium Free       | O(1)         |

| Large Allocation  | O(OS)        |

| Large Free        | O(OS)        |



\---



\# Design Characteristics



\## Strengths



\* Fast fixed-size allocation

\* Constant-time free operations

\* Low metadata overhead

\* Automatic coalescing in medium heap

\* Minimal reliance on OS allocations

\* Clear separation of allocation strategies

\* Fully custom implementation



\## Tradeoffs



\* Not thread-safe

\* No per-thread caches

\* No lock-free structures

\* Medium heap ordering is heuristic rather than exact

\* Less optimized than production allocators such as jemalloc, mimalloc, or tcmalloc



\---



\# Educational Value



RefAlloc was built primarily as a learning project.



The allocator demonstrates many techniques used by real-world allocators:



\* Free lists

\* Size classes

\* Page management

\* Boundary tags

\* Block splitting

\* Coalescing

\* Fragmentation management

\* Virtual memory interaction



while remaining compact enough to understand as a complete system.



The project serves both as a functional allocator and as an exploration of allocator design from first principles.



