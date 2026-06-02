#include "cstdio"
#include "refalloc.h"
#include "assert.h"
#include <cstddef>

/*
* `rnew` must, by design, always return a memory
* aligned to the largest `primitive` type.
* Currently, this type is 16 bytes wide.
* Therefore the alignment which we need to support
* for every allocation is also 16 byte.
*/
constexpr size_t MAX_ALIGNMENT = 16;

// The amount of free pages to always keep reserved.
constexpr size_t NUM_FREE_PAGES_RESERVE = 1;

/*
* Fixed-size allocation areas.
* 
* Each area is geared towards allocating and efficiently
* reusing small blocks of memory, with varying sizes.
* 
* Allocation and freeing such objects was optimized for O(1).
* Each area usually reserves exactly 1 virtual allocation as
* hysteresis, therefore avoiding repeating de/allocations of virtual
* memory from the OS on user memory boundaries.

* NOTE:
* Area with `size <= 8` suffer from possible misalignment.
* However, if you request objects aligned to 16 due to their size,
* you will receive an object from area 16 or larger so this ok.
* Also, block size 8 is the smallest which we can support because
* the struct MemoryBlock which is used as metadata has the minimum size of 8.
*/
static_assert(sizeof(MemoryBlock) <= 8, "MemoryBlock can't be larger than 8 bytes.");
Area area8;
Area area16;
Area area32;
Area area64;
Area area128;
Area area192;
Area area256; 
// Area 512 exists for potentially large objects that need to exist.
// At worst case, 512/MAX_ALIGNMENT = 32. So a class/struct with 32 16-bit fields
// can exist.
Area area512;

constexpr size_t NUM_SMALL_AREAS = 8;
Area* specificAreas[NUM_SMALL_AREAS] = { &area8, &area16, &area32, &area64, &area128, &area192, &area256, &area512 };

size_t PAGE_SIZE = 0;
bool is_refalloc_initialized = false;

/*
* The medium heap.
* 
* This is geared towards objects which are too large for the largest
* area, but still objects that would be too wasteful to get a new virtual
* allocation for.
* 
* For example, for a virtual allocation of size 8192, and for incoming allocation
* request of 2048, we can fit 4 of those user requested allocaton within 1 virtual
* allocation.
* 
* Wasting virtual allocation would waste too many system resoures (memory, and performance).
*/

constexpr size_t MED_HEAP_BLOCK_SIZE = 256;
static_assert(sizeof(BlockSequence) + sizeof(BlockSequenceFooter) <= MED_HEAP_BLOCK_SIZE,
			  "Block sequence won't be able to fit within a medium heap block.");
size_t MED_HEAP_PAGE_ALLOC_SIZE;
Heap mediumHeap;
size_t mediumHeapAlignedHeaderSize;
size_t mediumHeapAllocAlignedHeaderSize;
size_t mediumHeapMaxAllocSize;

/*
* Large object area.
* Large objects are objects which are inherently larger than a full OS allocation size.
* These allocations are directly mapped to virtual memory pages provided by OS.
* 
* There is very minimal optimization that can be done here.
* Therfore allocations whose size is greater than a single page
* are going to be directly mapped to those pages.
* 
* The large object area simply holds the head to the linked list of these allocations.
* It basically doesn't do anything except keeping track of these allocations.
*/
LargeObjectArea loArea;
size_t loAlignedHeaderSize;


size_t get_page_size() {
	SYSTEM_INFO info;
	GetSystemInfo(&info);	

	// This is the boundary for virtual allocations on windows.
	return info.dwAllocationGranularity;
}

void init_area(Area* area) {
	area->freePages = NULL;
	area->fullPages = NULL;
	area->partialPages = NULL;	
}

void init_alloc() {	
	PAGE_SIZE = get_page_size();
	MED_HEAP_PAGE_ALLOC_SIZE = PAGE_SIZE;

	loArea.head = NULL;

	// The first page of each large object will contain the object header.
	// The base address of the page is guaranteed to be aligned, but to
	// maintain alignment after the header, we must find the minimum
	// alignment boundary after the header where the actual user data can begin.
	loAlignedHeaderSize = 0;	
	while (loAlignedHeaderSize < sizeof(LargeObject)) {
		loAlignedHeaderSize += MAX_ALIGNMENT;
	}

	// And we use the same method for the header of the medium heap.
	mediumHeapAlignedHeaderSize = 0;
	while (mediumHeapAlignedHeaderSize < sizeof(HeapPage)) {
		mediumHeapAlignedHeaderSize += MAX_ALIGNMENT;
	}

	mediumHeapAllocAlignedHeaderSize = 0;
	while (mediumHeapAllocAlignedHeaderSize < sizeof(BlockSequence)) {
		mediumHeapAllocAlignedHeaderSize += MAX_ALIGNMENT;
	}

	// From each page in the medium heap we chomp:
	// metadata for the heap page itself,
	// the metadata from a single block (if only 1 big alloc) and it's footer.
	// technically footer not really needed for 1 alloc, but its a small optimization.
	mediumHeapMaxAllocSize = MED_HEAP_PAGE_ALLOC_SIZE - mediumHeapAlignedHeaderSize - mediumHeapAllocAlignedHeaderSize - sizeof(BlockSequenceFooter);
	mediumHeapMaxAllocSize = 64000;

	mediumHeap.freePages = NULL;
	mediumHeap.partialPages = NULL;

	init_area(&area512);
	area512.blockSize = 512;

	init_area(&area256);
	area256.blockSize = 256;

	init_area(&area192);
	area192.blockSize = 192;

	init_area(&area128);
	area128.blockSize = 128;

	init_area(&area64);
	area64.blockSize = 64;

	init_area(&area32);
	area32.blockSize = 32;

	init_area(&area16);
	area16.blockSize = 16;

	init_area(&area8);
	area8.blockSize = 8;

	is_refalloc_initialized = true;
}

MemoryBlock* pop_free_block(FixedBlockPage* page) {
	MemoryBlock* freeBlock = page->freeHead;
	page->freeHead = freeBlock->next;

	page->numFreeBlocks--;

	return freeBlock;
}

void push_free_block(FixedBlockPage* page, MemoryBlock* block) {
	block->next = page->freeHead;
	page->freeHead = block;

	page->numFreeBlocks++;
}

FixedBlockPage* pop_page_head(Area* area, PageKind from) {
	FixedBlockPage* fromHead;

	switch (from)
	{
	case Free:
		fromHead = area->freePages;
		area->freePages = fromHead->next;
		break;
	case Partial:
		fromHead = area->partialPages;
		area->partialPages = fromHead->next;
		break;
	case Full:
		fromHead = area->fullPages;
		area->fullPages = fromHead->next;
		break;
	default:
		fromHead = NULL;
		break;
	}

	if (fromHead != NULL) {
		if (fromHead->next != NULL) {
			fromHead->next->prev = NULL;
		}

		fromHead->next = NULL;
		fromHead->kind = PageKind::None;
	}

	return fromHead;
}

/// <summary>
/// Pushes `page` to the beginning of the pages list within `area`
/// specified by `to`.
/// Note that `page` must not be part of a linked list.
/// This method only pushes it.
/// </summary>
/// <param name="area">The area.</param>
/// <param name="to">The kind of list within the area.</param>
/// <param name="page">The page to insert as the new head.</param>
void push_page_head(Area* area, PageKind to, FixedBlockPage* page) {
	switch (to)
	{
	case Free:
		page->next = area->freePages;
		if (area->freePages != NULL) {
			area->freePages->prev = page;
		}
		area->freePages = page;
		break;
	case Partial:
		page->next = area->partialPages;
		if (area->partialPages != NULL) {
			area->partialPages->prev = page;
		}
		area->partialPages = page;
		break;
	case Full:
		page->next = area->fullPages;
		if (area->fullPages != NULL) {
			area->fullPages->prev = page;
		}
		area->fullPages = page;
		break;
	default:
		break;
	}

	page->kind = to;
}

/// <summary>
/// Takes a block from the partial pages of an area.
/// </summary>
/// <param name="area">The area from which the block will be taken.</param>
/// <returns>A pointer to the memory block.</returns>
MemoryBlock* take_block_from_partial_page(Area* area) {
	FixedBlockPage* partialPagesHead = area->partialPages;
	MemoryBlock* freeBlock = pop_free_block(partialPagesHead);

	// If there are no more free blocks within this page
	// we have to move it to the full pages list.
	if (partialPagesHead->freeHead == NULL) {
		push_page_head(area, PageKind::Full, pop_page_head(area, PageKind::Partial));
	}

	return freeBlock;
}

/// <summary>
/// Take a block from the free pages of the area.
/// </summary>
/// <param name="area">The area from which a block will be taken.</param>
/// <returns>The pointer to the memory block.</returns>
MemoryBlock* take_block_from_free_page(Area* area) {
	FixedBlockPage* freePagesHead = area->freePages;
	MemoryBlock* freeBlock = pop_free_block(freePagesHead);
	
	// If we take a block from a free page, it always becomes a partial page.
	push_page_head(area, PageKind::Partial, pop_page_head(area, PageKind::Free));
	return freeBlock;
}

/// <summary>
/// Perform actually allocation of virtual memory using
/// the underlying windows api.
/// </summary>
/// <returns>
/// A pointer to the beginning of the allocation,
/// interpreted as the page struct.
/// 
/// Although we return Page*, the actual amount of bytes
/// allocated is PAGE_SIZE.
/// </returns>
void* os_alloc(size_t numBytes) {
	LPVOID pMemory = VirtualAlloc(
		NULL,                       // System chooses the address
		numBytes,					// Size of allocation
		MEM_COMMIT | MEM_RESERVE,   // Allocate and commit concurrently
		PAGE_READWRITE              // Read/write access
	);

	if (pMemory == 0) {
		return NULL;
	}

	void* page = (void*)pMemory;
	return page;
}

void os_dealloc(void* allocBegin) {
	if (!VirtualFree((LPVOID)allocBegin, 0, MEM_RELEASE)) {
		printf("Freeing failed, error code: %d", GetLastError());
	}
}

/// <summary>
/// Initializes the list of free memory blocks within the page.
/// The first block is skipped as it is reserved for the header of the page.
/// The rest of the blocks are within the linked list.
/// </summary>
/// <param name="page"></param>
/// <param name="pageSize"></param>
/// <param name="blockSize"></param>
void init_new_os_page_memory_blocks_list(FixedBlockPage* page, size_t pageSize, size_t blockSize) {
	char* pageStart = (char*)page;
	char* pageEnd = pageStart + pageSize;

	size_t firstBlockOffset = 0;
	while (firstBlockOffset < sizeof(FixedBlockPage)) {
		firstBlockOffset += blockSize;
	}

	// Set the head to the initial position of the first memory block.
	page->freeHead = (MemoryBlock*) (pageStart + firstBlockOffset);
	page->numFreeBlocks = 1;

	// The potential position of the next memory block.
	char* currentOffset = pageStart + firstBlockOffset + blockSize;

	// Initialize the linked list of the rest of the memory blocks
	// within this page.
	MemoryBlock* previous = page->freeHead;
	
	// As long as we can fit another block at the current offset...
	while (currentOffset + blockSize <= pageEnd) {
		MemoryBlock* current = (MemoryBlock*)currentOffset;
		previous->next = current;
		previous = current;

		page->numFreeBlocks++;
		currentOffset += blockSize;
	}

	// Last block has no next.
	previous->next = NULL;
}

/*
* Attempts to allocate a block of memory with the size of `area->blockSize`.
* 
* First attempts to get it from a partial page.
* If there are no partial pages left, it will
* attempt to allocate it within a free page.
* 
* If there are no free pages available, it will
* allocate a new page and then allocate it within
* the new page.
* 
* area: the area from which a block will be attempted to be allocated.
*/
void* alloc_fixed_size(Area* area) {	
	if (area->partialPages != NULL) {
		// TODO: maybe zero memory in debug?
		// memset(freeBlock, 0, area->blockSize);
		return (void*)take_block_from_partial_page(area);
	}

	// If there aren't any free pages left, allocate a new one.
	if (area->freePages == NULL) {
		FixedBlockPage* newPage = (FixedBlockPage*)os_alloc(PAGE_SIZE);		
		if (newPage == NULL) {
			return NULL;
		}

		memset((void*)newPage, 0, sizeof(FixedBlockPage)); // Just in case.

		// Initialize the new page.		
		init_new_os_page_memory_blocks_list(newPage, PAGE_SIZE, area->blockSize);	
		newPage->pageAssociation = AllocationKind::FixedSizeArea;
		newPage->numTotalBlocks = newPage->numFreeBlocks;
		newPage->parentArea = area;		
		newPage->next = NULL;
		newPage->prev = NULL;
		newPage->kind = PageKind::Free;

		// Insert the new page into the empty list.
		area->freePages = newPage;
	}

	// If we are here then it must mean there is a free page.
	return (void*)take_block_from_free_page(area);
}

BlockSequenceFooter* get_block_seq_footer_ptr(BlockSequence* blockSeq) {
	return (BlockSequenceFooter*)((char*)blockSeq + blockSeq->numBlocks * MED_HEAP_BLOCK_SIZE - sizeof(BlockSequenceFooter));
}

BlockSequence* get_block_seq_ptr(BlockSequenceFooter* footer) {
	return (BlockSequence*)(((char*)footer + sizeof(BlockSequenceFooter)) - (footer->numBlocks * MED_HEAP_BLOCK_SIZE));
}

void detach_block_seq(BlockSequence* seq) {
	if (seq->prev != NULL) {
		seq->prev->next = seq->next;
	}

	if (seq->next != NULL) {
		seq->next->prev = seq->prev;
	}

	seq->next = NULL;
	seq->prev = NULL;
}

void heuristic_push_partial_page_back() {
	if (mediumHeap.partialPages == NULL || mediumHeap.partialPages->next == NULL) {
		// No page / next page either way...
		return;
	}

	size_t firstPageMax = 0;
	if (mediumHeap.partialPages->blockSeq != NULL) {
		firstPageMax = mediumHeap.partialPages->blockSeq->numBlocks;
	}

	size_t nextPageMax = 0;
	if (mediumHeap.partialPages->next->blockSeq != NULL) {
		nextPageMax = mediumHeap.partialPages->next->blockSeq->numBlocks;
	}

	// Heurstically okay
	if (firstPageMax >= nextPageMax) {
		return;
	}

	// Swap
	HeapPage* cur = mediumHeap.partialPages;
	HeapPage* third = cur->next->next;

	mediumHeap.partialPages = cur->next;
	mediumHeap.partialPages->next = cur;
	cur->next = third;
	cur->prev = mediumHeap.partialPages;

	if (third != NULL) {
		third->prev = cur;
	}

	mediumHeap.partialPages->prev = NULL;
}

void* alloc_heap(size_t numBytes) {	
	size_t totalRequiredBytes = (mediumHeapAllocAlignedHeaderSize + numBytes + sizeof(BlockSequenceFooter));
	size_t numRequiredBlocks = totalRequiredBytes / MED_HEAP_BLOCK_SIZE;
	if (totalRequiredBytes % MED_HEAP_BLOCK_SIZE != 0) {
		numRequiredBlocks++;
	}

	size_t numBlocksInFirst = 0;
	if (mediumHeap.partialPages != NULL && mediumHeap.partialPages->blockSeq != NULL) {
		numBlocksInFirst = mediumHeap.partialPages->blockSeq->numBlocks;
	}

	bool firstPartialPageHasEnoughMemory = mediumHeap.partialPages != NULL && numBlocksInFirst >= numRequiredBlocks;
	bool hasFreePage = mediumHeap.freePages != NULL;

	// If there is not head yet, we have to allocate.
	// Secondly, the pages in the medium heap are sorted in descending order
	// based on the longest block sequence within the page itself.
	// Therefore, if the longest block sequence within the head is also less
	// than the number of required blocks, we also have to allocate.
	// That is because within any other page, the longest sequence of unused blocks
	// will be by the invariant less than this one.
	if (!firstPartialPageHasEnoughMemory && !hasFreePage) {
		size_t bytesToAllocate = PAGE_SIZE;
		void* osMem = os_alloc(bytesToAllocate);
		if (osMem == NULL) {
			return NULL; // out of virtual memory.
		}

		HeapPage* page = (HeapPage*)osMem;
		page->pageAssociation = AllocationKind::MediumHeap;
		page->next = mediumHeap.freePages;
		page->prev = NULL;
		if (mediumHeap.freePages != NULL) {
			mediumHeap.freePages->prev = page;
		}

		char* firstBlockSeqAddress = (char*)page + mediumHeapAlignedHeaderSize;
		page->blockSeq = (BlockSequence*)((char*)page + mediumHeapAlignedHeaderSize);

		// Round the block index up to `MED_HEAP_BLOCK_SIZE`.
		page->blockSeq->beginBlock = mediumHeapAlignedHeaderSize / MED_HEAP_BLOCK_SIZE;
		if (mediumHeapAlignedHeaderSize % MED_HEAP_BLOCK_SIZE > 0) {
			page->blockSeq->beginBlock++;
		}
		page->blockSeqFirstBlock = page->blockSeq->beginBlock;

		// The number of available blocks within a newly allocated page.
		size_t totalFreeBlocks = (size_t)((bytesToAllocate - mediumHeapAlignedHeaderSize) / MED_HEAP_BLOCK_SIZE);
		page->blockSeq->numBlocks = totalFreeBlocks;
		page->blockSeq->next = NULL;
		page->blockSeq->prev = NULL;
		page->totalFreeBlocks = totalFreeBlocks;
		page->totalBlocks = bytesToAllocate / MED_HEAP_BLOCK_SIZE;
		page->prev = NULL;
		page->blockSeq->used = false;

		mediumHeap.freePages = page;
		mediumHeap.numFreePages++;
	}

	// The page from which we take the block is the head.
	HeapPage* selectedPage;
	if (firstPartialPageHasEnoughMemory) {
		selectedPage = mediumHeap.partialPages;
	}
	else {
		// We got here, but the first partial page
		// doesn't have enough memory. This must mean
		// there is at least 1 free page available.
		selectedPage = mediumHeap.freePages;
	}

	// The block we take from the page is the head.
	BlockSequence* blockSeq = selectedPage->blockSeq;

	// If we are here it is guaranteed that the block sequence of the head
	// has enough blocks.
	assert(blockSeq->numBlocks >= numRequiredBlocks);
	
	// Number of blocks in the right split.
	size_t numRightBlocks = blockSeq->numBlocks - numRequiredBlocks;	

	// Split into right block if we can.	
	if (numRightBlocks != 0) {
		// Instantiate the right block in-place.
		BlockSequence* rightSeq = (BlockSequence*)((char*)blockSeq + numRequiredBlocks * MED_HEAP_BLOCK_SIZE);
		rightSeq->beginBlock = blockSeq->beginBlock + numRequiredBlocks;
		rightSeq->numBlocks = numRightBlocks;
		rightSeq->used = false;

		rightSeq->next = blockSeq->next;
		if (rightSeq->next != NULL) {
			rightSeq->next->prev = rightSeq;
		}

		// blockSeq->prev is NULL because it is the head.
		// We will set rightSeq as the head, so it will
		// simply inherit the NULL prev.
		rightSeq->prev = NULL;
	
		BlockSequenceFooter* rightSeqFooter = get_block_seq_footer_ptr(rightSeq);
		rightSeqFooter->numBlocks = numRightBlocks;

		// Set right block as head.
		selectedPage->blockSeq = rightSeq;
	}

	// Update info of the left split, which is our block.
	blockSeq->numBlocks = numRequiredBlocks;		
	get_block_seq_footer_ptr(blockSeq)->numBlocks = numRequiredBlocks;	
	blockSeq->prev = NULL;	
	blockSeq->used = true;

	// If there was no right split,
	// we assign the head as the next
	// of the current block, and detach the one we popped.
	if (numRightBlocks == 0) {
		selectedPage->blockSeq = blockSeq->next;
		if (selectedPage->blockSeq != NULL) {
			selectedPage->blockSeq->prev = NULL;
		}
	}
	blockSeq->next = NULL;

	// If it was free, now it isn't.
	// Push to partial.
	if (selectedPage == mediumHeap.freePages) {
		mediumHeap.freePages = mediumHeap.freePages->next;
		if (mediumHeap.freePages != NULL) {
			mediumHeap.freePages->prev = NULL;
		}

		selectedPage->next = mediumHeap.partialPages;
		selectedPage->prev = NULL;
		if (mediumHeap.partialPages != NULL) {
			mediumHeap.partialPages->prev = selectedPage;
		}
		mediumHeap.partialPages = selectedPage;

		mediumHeap.numFreePages--;
	}

	heuristic_push_partial_page_back();

	return (void*)((char*)blockSeq + mediumHeapAllocAlignedHeaderSize);
}

void dealloc_heap(HeapPage* page, void* ptr) {
	// Restore the block sequence header.
	// Until pushing `block` back into the list,
	// we are going to make sure that this will be detached.
	BlockSequence* block = (BlockSequence*)((char*)ptr - mediumHeapAllocAlignedHeaderSize);

	// This block is the last one if it ends the page.
	// And if it the block with the highest address, obviously
	// there are no more blocks with higher addresses.
	// Therefore there exists a block to the right ONLY if it
	// is not the last.
	if (block->beginBlock + block->numBlocks != page->totalBlocks) {
		// So this isn't the last block sequence, lets check if we can merge with right block.
		BlockSequence* rightSeq = (BlockSequence*)((char*)block + block->numBlocks * MED_HEAP_BLOCK_SIZE);
		if (!rightSeq->used) {
			// Right block not used, we can merge.
			// 
			// We want to detach the merged block from the list
			// to keep `block` detached for simplicity.
			// Make sure right block is not the head.
			if (rightSeq == page->blockSeq) {
				// Detach so that if rightSeq was head,
				// now it won't be.
				page->blockSeq = rightSeq->next;
				if (rightSeq->next != NULL) {
					rightSeq->next->prev = NULL;
				}
				rightSeq->next = NULL;
			}

			block->numBlocks += rightSeq->numBlocks;			
			get_block_seq_footer_ptr(rightSeq)->numBlocks = block->numBlocks;

			// Detach the block.
			// This is correct for a general linked list,
			// and we correctly managed the case that `rightSeq`
			// was head up top.
			detach_block_seq(rightSeq);
		}
	}

	// The block is the first one only if it's index
	// is literally the first block in the page.
	// The first block obviously has no one with lower
	// address. So only if it isn't the first block
	// can there be a block with lower address to merge.
	if (block->beginBlock != page->blockSeqFirstBlock) {
		BlockSequenceFooter* leftSeqFooter = (BlockSequenceFooter*)((char*)block - sizeof(BlockSequenceFooter));
		BlockSequence* leftSeq = get_block_seq_ptr(leftSeqFooter);
		if (!leftSeq->used) {
			// Left block not used, we can merge.
			// 
			// We want to detach the merged block from the list
			// to keep `block` detached for simplicity.
			// Make sure left block is not the head.
			if (leftSeq == page->blockSeq) {
				// Detach so that if leftSeq was head,
				// now it won't be.
				page->blockSeq = leftSeq->next;
				if (leftSeq->next != NULL) {
					leftSeq->next->prev = NULL;
				}
				leftSeq->next = NULL;
			}

			// To merge from left:
			// note that `block` is guaranteed to be a valid, detached block.
			// Also detach left, and merge them into a single bigger detached block.
			detach_block_seq(leftSeq);

			// The left block just consumes us.
			leftSeq->numBlocks += block->numBlocks;			
			get_block_seq_footer_ptr(block)->numBlocks = leftSeq->numBlocks;

			// This is the final block.
			block = leftSeq;
		}
	}

	// Right now whatever is in `block`
	// is a valid but detached block.
	if (page->blockSeq == NULL) {
		// If no head exists, then the single block is the head.
		page->blockSeq = block;
	}
	else {
		// Some head exists.
		// Heuristic: if our first block is larger than his, we head.
		// Otherwise, we will be his direct descendant.
		if (block->numBlocks > page->blockSeq->numBlocks) {
			block->next = page->blockSeq;
			block->prev = NULL;
			page->blockSeq->prev = block;
			page->blockSeq = block;
		}
		else {
			// Insert block after the first block.

			// Configure block in the middle.
			block->next = page->blockSeq->next;
			block->prev = page->blockSeq;

			// Fix `prev` of the 3rd.
			if (block->next != NULL) {
				block->next->prev = block;
			}

			// Fix first.
			page->blockSeq->next = block;
		}
	}

	// And finally, mark block as unused.
	block->used = false;

	// Unlink the page from the partial pages.
	if (page == mediumHeap.partialPages) {
		mediumHeap.partialPages = mediumHeap.partialPages->next;
		if (mediumHeap.partialPages != NULL) {
			mediumHeap.partialPages->prev = NULL;
		}
	}
	else {
		if (page->next == NULL) {
			// tail...
			page->prev->next = NULL;
		}
		else {
			// middle
			page->prev->next = page->next;
			page->next->prev = page->prev;
		}
	}

	// If page is completely free, unlink from the partial pages.
	// and push into the free page list.	
	if (page->blockSeq->numBlocks == page->totalFreeBlocks) {
		page->next = mediumHeap.freePages;
		page->prev = NULL;
		if (mediumHeap.freePages != NULL) {
			mediumHeap.freePages->prev = page;
		}
		mediumHeap.freePages = page;
		mediumHeap.numFreePages++;

		while (mediumHeap.numFreePages > NUM_FREE_PAGES_RESERVE) {
			HeapPage* pageToDealloc = mediumHeap.freePages;
			mediumHeap.freePages = mediumHeap.freePages->next;
			mediumHeap.freePages->prev = NULL;

			os_dealloc((void*)pageToDealloc);
			mediumHeap.numFreePages--;
		}
	}
	else {
		// Push as head of partial pages.
		page->next = mediumHeap.partialPages;
		page->prev = NULL;
		if (mediumHeap.partialPages != NULL) {
			mediumHeap.partialPages->prev = page;
		}
		mediumHeap.partialPages = page;
		heuristic_push_partial_page_back();
	}
}

void* alloc_large_object(size_t numBytes) {
	size_t totalSizeNeeded = numBytes + loAlignedHeaderSize;	
	LargeObject* lo = (LargeObject*)os_alloc(totalSizeNeeded);
	if (lo == NULL) {
		return NULL;
	}

	lo->pageAssociation = AllocationKind::LargeObjectHeap;
	lo->dataHead = (char*)lo + loAlignedHeaderSize;
	lo->objectSize = numBytes;	

	// Insert the large object into the list.
	lo->prev = NULL;
	lo->next = loArea.head;
	if (loArea.head != NULL) {
		loArea.head->prev = lo;
	}
	loArea.head = lo;

	return lo->dataHead;
}

void dealloc_large_object(void* ptr) {
	LargeObject* lo = (LargeObject*)((char*)ptr - loAlignedHeaderSize);
	
	// If its the head
	if (lo == loArea.head) {
		loArea.head = lo->next;

		// Reset previous of the next, only if it exists.
		if (lo->next != NULL) {
			lo->next->prev = NULL;
		}		
	}
	else if (lo->next == NULL) {
		// It is the tail.
		// Note that it's guaranteed that `lo->prev != NULL`,
		// because `lo->prev == NULL` only for the head, which
		// is the previous case.
		lo->prev->next = NULL;
	}
	else {
		// We neither the head, nor the tail.
		lo->prev->next = lo->next;
		lo->next->prev = lo->prev;
	}

	os_dealloc((void*)lo);
}

void* do_alloc(size_t numBytes) {
	if (numBytes > mediumHeapMaxAllocSize) {
		return alloc_large_object(numBytes);
	}

	if (numBytes > specificAreas[NUM_SMALL_AREAS - 1]->blockSize) {
		return alloc_heap(numBytes);
	}

	// Linear scan to find the area with the smallest block
	// size that can contain `numBytes`.
	Area* bestFitArea = NULL;
	for (size_t i = 0; i < NUM_SMALL_AREAS; i++) {
		Area* area = specificAreas[i];
		if (numBytes <= area->blockSize) {
			bestFitArea = area;
			break;
		}
	}

	return alloc_fixed_size(bestFitArea);
}

void* rnew(size_t numBytes) {
	if (!is_refalloc_initialized) {
		init_alloc();
	}

	return do_alloc(numBytes);
};

size_t count_page_list(FixedBlockPage* head) {
	size_t count = 0;
	while (head != NULL) {
		count++;
		head = head->next;
	}
	return count;
}

void dealloc_small_object(FixedBlockPage* page, void* userPtr) {	
	// TODO: support this in debug mode.
	// Technically, we can just cast `(MemoryBlock*)userPtr`,
	// but this in theory let's us do a check in debug if the user ptr
	// actually belong to the page.
	// char* blockOffset = (char*) ((char*)userPtr - (char*)page);
	// MemoryBlock* block = (MemoryBlock*) ((char*)page + (size_t)blockOffset);
	push_free_block(page, (MemoryBlock*)userPtr);

	if (page->numFreeBlocks == page->numTotalBlocks) {
		// All the memory blocks within the page are free.
		// Therefore we need to move this page to the free pages
		// list of the area.
		// First unlink the page from the list:
		FixedBlockPage* prev = page->prev;
		FixedBlockPage* next = page->next;

		// If this page is head of the page list
		if (prev == NULL) {			
			push_page_head(page->parentArea, PageKind::Free, pop_page_head(page->parentArea, page->kind));
		}
		else {
			// It somewhere in the middle, or it is the tail.
			if (next == NULL) {
				// If it is the tail, remove it
				prev->next = NULL;
				page->prev = NULL;				
			}
			else {
				// If it is in the middle.
				prev->next = next;
				next->prev = prev;
			}

			// Now doesn't belong to anyone.
			// Safely push it as the head.
			push_page_head(page->parentArea, PageKind::Free, page);
		}

		size_t numFreePages = count_page_list(page->parentArea->freePages);
		while (numFreePages > NUM_FREE_PAGES_RESERVE) {
			FixedBlockPage* freePage = pop_page_head(page->parentArea, PageKind::Free);
			os_dealloc(freePage);
			numFreePages--;
		}
	}
}

void rfree(void* ptr) {
	size_t userPtr = (size_t)ptr;
	userPtr /= PAGE_SIZE;
	userPtr *= PAGE_SIZE;

	// Within each OS allocation, the first sizeof(PageAssociation)
	// are reserved so we know what page association this page belongs to.
	// This is true for large object allocations, where `ptr` is expected
	// to point the beginning of the OS allocation which has this.
	// And this is also true for all fixed size areas, where the header of
	// each page also contains this value.
	AllocationKind pageAssociation = *((AllocationKind*)userPtr);

	if (pageAssociation == AllocationKind::FixedSizeArea) {
		dealloc_small_object((FixedBlockPage*)userPtr, ptr);
	}
	else if (pageAssociation == AllocationKind::MediumHeap) {
		dealloc_heap((HeapPage*)userPtr, ptr);
	}
	else if (pageAssociation == AllocationKind::LargeObjectHeap) {
		dealloc_large_object(ptr);
	}
}