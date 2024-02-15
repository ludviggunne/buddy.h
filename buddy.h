
/**
 *  Buddy allocator. Do not use in conjunction with
 *  malloc / free.
 *
 *  Usage:
 *
 *      #define BUDDY_IMPLEMENTATION
 *      #include "buddy.h"
 *
 *      int main()
 *      {
 *          char *ptr = balloc(sizeof(*ptr));
 *          *ptr = 'X';
 *          bfree(ptr);
 *      }
 */

#ifndef BUDDY_H
#define BUDDY_H

#ifdef BUDDY_STDLIB_OVERRIDE
    #include <stdlib.h>
    #define BNULL    NULL
    #define balloc   malloc
    #define bfree    free
    #define brealloc realloc
    #define bcalloc  calloc
#else
    #define BNULL ((void *) 0)
#endif

#include <stddef.h>


/**
 *  Allocate `size` bytes of memory.
 *  Returns BNULL on failure.
 */
void *balloc(size_t size);

/**
 *  Free previously allocated memory.
 */
void bfree(void *ptr);

/**
 *  Attempt to reallocate memory to fit new size.
 *  Returns BNULL on failure.
 */
void *brealloc(void *ptr, size_t size);

/**
 *  Allocate memory for `nitems` objects of size `size`,
 *  and zero memory. Returns BNULL on failure.
 */
void *bcalloc(size_t nitems, size_t size);

#endif



#ifdef BUDDY_IMPLEMENTATION
#undef BUDDY_IMPLEMENTATION

#include <stdint.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <pthread.h>

typedef uint8_t byte_t;

struct block {
    size_t size;
    int used;
    _Alignas(max_align_t) byte_t mem[];
};

#ifndef BUDDY_BLOCK_INIT_SIZE
#define BUDDY_BLOCK_INIT_SIZE 4096
#endif

_Static_assert((BUDDY_BLOCK_INIT_SIZE & (BUDDY_BLOCK_INIT_SIZE - 1)) == 0,
               "buddy.h: BUDDY_BLOCK_INIT_SIZE must be a power of two.");



// find next block
#define NEXT(block_ptr)\
    (struct block *)((byte_t*)(block_ptr) + (block_ptr)->size)
// the offset of the usable memory region in a block
#define MEMOFFSET (offsetof(struct block, mem))
// the size of usable memory `block_ptr` can hold
#define MEMSIZE(block_ptr) (size_t)((block_ptr)->size - MEMOFFSET)
// get pointer to block containing `mem`
#define BLOCK(mem) (struct block *)((byte_t *)mem - MEMOFFSET)
// the size of usable memory if we would split `block_ptr`
#define HALFMEMSIZE(block_ptr)\
    (size_t)((block_ptr)->size / 2 - MEMOFFSET)
// the smallest size a block can be
#define MINBLOCKSIZE 16
// the size of a block that can hold `memsize` bytes
// of usable memory
#define BLOCKSIZE(memsize) (memsize + MEMOFFSET)
// the byte offset between two pointers
#define BYTEDIFF(ptr1, ptr2)\
    (size_t) ((byte_t *) ptr2 - (byte_t *) ptr1)


// points to the first block in memory
static struct block *start;
// points to the end of last block in memory
static struct block *end;
// points to the next block to consider for allocation
static struct block *next;
// lock for the whole allocator
static pthread_mutex_t lock;


#ifdef BUDDY_STDLIB_OVERRIDE
// we call bfree (=free when this ^^^ is defined)
// and then use the pointer in some functions
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wuse-after-free"
#endif

__attribute__((constructor))
static void init(void)
{
    pthread_mutexattr_t lock_attr;
    // lock may be aquired multiple times,
    // for example when brealloc calls balloc
    pthread_mutexattr_settype(&lock_attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&lock, &lock_attr);
    pthread_mutex_lock(&lock);

    // setup an inital block of size BUDDY_BLOCK_INIT_SIZE
    start = sbrk(0);

    // adjust alignment if necessary
    const size_t A = _Alignof(max_align_t);
    if ((size_t) start % A > 0)
    {
        (void) sbrk(A - (size_t) start % A);
        start = sbrk(0);
    }

    if (sbrk(BUDDY_BLOCK_INIT_SIZE) == (void *) -1)
    {
        assert(0 && "failed to initialize buddy.h");
    }
    end = (struct block *)
        ((byte_t *) start + BUDDY_BLOCK_INIT_SIZE);

    next = start;
    next->size = BUDDY_BLOCK_INIT_SIZE;
    next->used = 0;
    pthread_mutex_unlock(&lock);
}

static struct block *grow(size_t required)
{
    required = BLOCKSIZE(required);
    size_t current_size;
    struct block *block;

    // if there is just one free block,
    // just grow that
    if (NEXT(start) == end && !start->used)
    {
        size_t size = start->size;
        while (size < required)
        {
            size *= 2;
        }

        if (sbrk(size - start->size) == (void *) -1)
        {
            return BNULL;
        }

        start->size = size;
        end = NEXT(start);

        return start;
    }

    // keep growing until last block is big enough
    do {
        current_size = BYTEDIFF(start, end);

        if (sbrk(current_size) == (void *) -1)
        {
            return BNULL;
        }

        block = end;
        block->size = current_size;
        block->used = 0;

        end = NEXT(block);

    } while (current_size < required);

    return block;
}

static void split(struct block *block)
{
    block->size /= 2;
    struct block *next = NEXT(block);
    next->size = block->size;
    next->used = 0;
}

static struct block *join(struct block *block)
{
    struct block *buddy, *joined;
    size_t size = block->size;

    for (;;)
    {
        if (BYTEDIFF(start, block) % (size * 2) == 0)
        {
            buddy = (struct block *) ((byte_t *) block + size);
            joined = block;
        }
        else
        {
            buddy = (struct block *) ((byte_t *) block - size);
            joined = buddy;
        }

        if (buddy == end ||
            buddy->size != size ||
            buddy->used)
        {
            break;
        }
        else
        {
            block = joined;
            size *= 2;
        }
    }

    block->size = size;
    block->used = 0;
    return block;
}

void *balloc(size_t size)
{
    pthread_mutex_lock(&lock);

    struct block *block = next;

    // search for unused block that fits allocation
    while (MEMSIZE(block) < size || block->used)
    {

        block = NEXT(block);

        // wrap around
        if (block == end)
        {
            block = start;
        }

        // went through all available blocks
        // try to grow
        if (block == next)
        {
            block = grow(size);
            if (block == BNULL)
            {
                // can't grow
                pthread_mutex_unlock(&lock);
                return BNULL;
            }
            break;
        }
    }

    // split until we have best fit
    while (HALFMEMSIZE(block) >= size && block->size > MINBLOCKSIZE)
    {
        split(block);
    }

    // record where we should start searching next
    next = NEXT(block);
    if (next == end)
    {
        next = start;
    }

    block->used = 1;
    pthread_mutex_unlock(&lock);
    return block->mem;
}

void bfree(void *ptr)
{
    if (ptr == BNULL)
    {
        return;
    }

    pthread_mutex_lock(&lock);
    struct block *block = BLOCK(ptr);
    block = join(block);
    next = block;
    pthread_mutex_unlock(&lock);
}

void *brealloc(void *ptr, size_t size)
{
    pthread_mutex_lock(&lock);
    struct block *block, *buddy;
    size_t block_size;
    byte_t *new_ptr;

    if (ptr == BNULL)
    {
        return balloc(size);
    }

    if (size == 0)
    {
        bfree(ptr);
        pthread_mutex_unlock(&lock);
        return BNULL;
    }

    block = BLOCK(ptr);

    if (MEMSIZE(block) >= size)
    {
        while ((block->size / 2 - MEMOFFSET) >= size)
        {
            split(block);
        }
        next = NEXT(block);
        pthread_mutex_unlock(&lock);
        return block->mem;
    }

    block_size = block->size;

    // try to grow current block by joining
    // with only right buddies
    for (;;)
    {
        if (block_size >= BLOCKSIZE(size))
        {
            block->size = block_size;
            next = NEXT(block);
            pthread_mutex_unlock(&lock);
            return block->mem;
        }

        buddy = (struct block *) ((byte_t *) block + block_size);

        if (BYTEDIFF(start, block) % (block_size * 2) > 0 ||
            buddy == end ||
            buddy->size != block_size ||
            buddy->used)
        {
            break;
        }

        block_size *= 2;
    }

    block_size = block->size;
    bfree(ptr);

    new_ptr = balloc(size);
    if (new_ptr == BNULL)
    {
        // restore freed block
        block->size = block_size;
        block->used = 1;
        pthread_mutex_unlock(&lock);
        return BNULL;
    }

    if (new_ptr < (byte_t *)ptr)
    {
        for (size_t i = 0; i < MEMSIZE(block); i++)
        {
            new_ptr[i] = ((byte_t *)ptr)[i];
        }
    }
    else
    {
        for (size_t i = MEMSIZE(block); i > 0; i--)
        {
            new_ptr[i - 1] = ((byte_t *)ptr)[i - 1];
        }
    }

    pthread_mutex_unlock(&lock);
    return new_ptr;
}

void *bcalloc(size_t nitems, size_t size)
{
    size *= nitems;
    char *ptr = balloc(size);
    if (ptr == BNULL)
    {
        return BNULL;
    }
    memset(ptr, 0, size);
    return ptr;
}

#ifdef BUDDY_STDLIB_OVERRIDE
#pragma GCC diagnostic pop
#endif

#endif
