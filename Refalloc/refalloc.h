#pragma once

#include "windows.h"
#include "memoryapi.h"
#include "stdint.h"

struct MemoryBlock {
	MemoryBlock* next;
};

struct Area;

enum AllocationKind {
	FixedSizeArea,
	MediumHeap,
	LargeObjectHeap,
};

enum PageKind {
	None,
	Free,
	Partial,
	Full,
};

struct FixedBlockPage {
	// Required association header for each page.
	AllocationKind pageAssociation;

	Area* parentArea;
	size_t numTotalBlocks;
	size_t numFreeBlocks;
	MemoryBlock* freeHead;
	FixedBlockPage* prev;
	FixedBlockPage* next;
	PageKind kind;
};

struct Area {
	FixedBlockPage* freePages;
	FixedBlockPage* partialPages;
	FixedBlockPage* fullPages;
	uint16_t blockSize;
};

struct BlockSequence {
	size_t beginBlock;
	size_t numBlocks;
	BlockSequence* next;
	BlockSequence* prev;
	bool used;
};

struct BlockSequenceFooter {
	size_t numBlocks; 
};

struct HeapPage {
	AllocationKind pageAssociation;
	BlockSequence* blockSeq;
	size_t blockSeqFirstBlock;
	size_t totalNumBlocks;
	HeapPage* next;
	HeapPage* prev;
};

struct Heap {
	size_t numFreePages;
	HeapPage* freePages;
	HeapPage* partialPages;
};

struct LargeObject {
	// Required association header for the first page
	// of the object.
	AllocationKind pageAssociation;

	char* dataHead;
	size_t objectSize;

	LargeObject* next;
	LargeObject* prev;
};

struct LargeObjectArea {
	LargeObject* head;
};

void* rnew(size_t bytes);
void rfree(void* ptr);


