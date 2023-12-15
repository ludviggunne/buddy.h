
#ifndef BUDDY_H
#define BUDDY_H

/**
 * Usage:
 *     #define BUDDY_IMPLEMENTATION
 *     #include "buddy.h"
 */

#include <stddef.h>

typedef struct block block_t;

typedef struct {
    block_t *start;
    block_t *stop;
} buddy_t;



/**
 * Initialize allocator with pre-allocated memory.
 * The size should optimally be equal to a power of two.
 */
buddy_t buddy_init(void *mem, size_t size);

/**
 * Allocate a new block of memory.
 * Returns NULL on fail
 */
void *buddy_alloc(buddy_t *buddy, size_t size);

/**
 * Free previously allocated memory.
 */
void buddy_free(buddy_t *buddy, void *ptr);



#endif /* BUDDY_H */







#ifdef BUDDY_IMPLEMENTATION
#undef BUDDY_IMPLEMENTATION



#include <stdint.h>
#ifdef BUDDY_DEBUG
#include <stdio.h>
#endif



/* the offset of the usable memory region in a block */
#define BLOCK_MEM_OFFSET (size_t)(&((struct block *)0)->mem)

/* get a pointer to the block containing usable memory pointed to by ptr */
#define MEM_BLOCK_PTR(ptr) (struct block *)((byte_t *)ptr - BLOCK_MEM_OFFSET)

/* size of usable memory in a block with given size */
#define MEM_SIZE(size) (size - BLOCK_MEM_OFFSET)

/* size of block containing usable memory of given size */
#define BLOCK_SIZE(size) (size + BLOCK_MEM_OFFSET)

/* the smallest size for a single block */
#define BLOCK_MIN_SIZE (BLOCK_MEM_OFFSET + sizeof(word_t))

/* offset a block pointer by given number of bytes */
#define BLOCK_BYTE_OFFSET(block, offset) ((block_t *)((byte_t *)(block) + (offset)))



typedef intptr_t word_t;
typedef uint8_t byte_t;



struct block
{
    /* HEADER */
    size_t size;
    byte_t free;
    /* MEMORY */
    word_t mem[];
};



/* FORWARD DECLARATIONS */

static block_t *get_next(block_t *block, block_t *stop);
static block_t *split(block_t *block);
static size_t p2align_down(size_t size);
static block_t *try_merge(block_t *start, block_t *block, block_t *stop);



/* API */

buddy_t buddy_init(void *mem, size_t size)
{
    size = p2align_down(size);

    block_t *block = mem;
    block->size = size;
    block->free = 1;

    buddy_t buddy;
    buddy.start = block;
    buddy.stop = BLOCK_BYTE_OFFSET(block, size);

    return buddy;
}



void *buddy_alloc(buddy_t *buddy, size_t size)
{
    if (size == 0)
    {
        return NULL;
    }

    block_t *block = buddy->start;

    /* find free block */
    while (!block->free || MEM_SIZE(block->size) < size)
    {
        block = get_next(block, buddy->stop);
        if (block == NULL)
        {
            return NULL;
        }
    }

    /* find smallest block that will fit allocation */
    while (MEM_SIZE(block->size / 2) >= size)
    {
        block_t *new_block = split(block);
        if (new_block == NULL)
        {
            break;
        }
        block = new_block;
    }

    block->free = 0;
    return &block->mem;
}



void buddy_free(buddy_t *buddy, void *ptr)
{
    block_t *block = MEM_BLOCK_PTR(ptr);
    while ((block = try_merge(buddy->start, block, buddy->stop)));
}



/* INTERNALS */

/* determines if the block is left or right buddy */
/* from its size and offset from start block */
static int is_left(block_t *start, block_t *block)
{
    size_t offset = (byte_t *)block - (byte_t *)start;
    return offset % (block->size * 2) == 0;
}



/* returns the block resulting from merge */
/* returns NULL if merge is not possible */
static block_t *try_merge(block_t *start, block_t *block, block_t *stop)
{

    block_t *buddy;

    const int left = is_left(start, block);

    if (left)
    {
        buddy = BLOCK_BYTE_OFFSET(block, block->size);
    }
    else
    {
        buddy = BLOCK_BYTE_OFFSET(block, -block->size);
    }

    const int no_merge =
        /* can't merge root block */
        buddy >= stop ||
        /* can't merge with used block */
        !buddy->free ||
        /* can't merge with block of different size */
        buddy->size != block->size;

    if (no_merge)
    {
        return NULL;
    }

    block_t *new_block;

    if (left)
    {
        new_block = block;
    }
    else
    {
        new_block = buddy;
    }

    new_block->size = block->size * 2;
    new_block->free = 1;

    return new_block;
}



static block_t *split(block_t *block)
{
    if (block->size / 2 < BLOCK_MIN_SIZE)
    {
        return NULL;
    }

    block->size /= 2;

    /* split block can't be root -> don't check for overflow */
    block_t *buddy = get_next(block, NULL);

    buddy->size = block->size;
    buddy->free = 1;

    return block;
}



/* get the next block in the structure */
/* doesn't check for overflow if stop is NULL */
static block_t *get_next(block_t *block, block_t *stop)
{
    block_t *next_block = BLOCK_BYTE_OFFSET(block, block->size);

    if (stop != NULL && next_block >= stop)
    {
        return NULL;
    }

    return next_block;
}



/* find the biggest power of 2 that fits in size */
static size_t p2align_down(size_t size)
{
    size--;
    size |= size >> 1;
    size |= size >> 2;
    size |= size >> 4;
    size |= size >> 8;
    size |= size >> 16;
    size |= size >> 32;
    size++;
    size >>= 1;
    return size;
}



#undef BLOCK_MEM_OFFSET
#undef MEM_BLOCK_PTR
#undef MEM_SIZE
#undef BLOCK_SIZE
#undef BLOCK_MIN_SIZE
#undef BLOCK_BYTE_OFFSET

#endif /* BUDDY_IMPLEMENTATION */
