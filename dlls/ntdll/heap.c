/*
 * Win32 heap functions
 *
 * Copyright 1996 Alexandre Julliard
 * Copyright 1998 Ulrich Weigand
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <assert.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define RUNNING_ON_VALGRIND 0  /* FIXME */

#include "ntstatus.h"
#define WIN32_NO_STATUS
#define NONAMELESSUNION
#include "windef.h"
#include "winnt.h"
#include "winternl.h"
#include "ntdll_misc.h"
#include "wine/list.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(heap);

/* undocumented RtlWalkHeap structure */

struct rtl_heap_entry
{
    LPVOID lpData;
    SIZE_T cbData; /* differs from PROCESS_HEAP_ENTRY */
    BYTE cbOverhead;
    BYTE iRegionIndex;
    WORD wFlags; /* value differs from PROCESS_HEAP_ENTRY */
    union {
        struct {
            HANDLE hMem;
            DWORD dwReserved[3];
        } Block;
        struct {
            DWORD dwCommittedSize;
            DWORD dwUnCommittedSize;
            LPVOID lpFirstBlock;
            LPVOID lpLastBlock;
        } Region;
    };
};

/* rtl_heap_entry flags, names made up */

#define RTL_HEAP_ENTRY_BUSY         0x0001
#define RTL_HEAP_ENTRY_REGION       0x0002
#define RTL_HEAP_ENTRY_BLOCK        0x0010
#define RTL_HEAP_ENTRY_UNCOMMITTED  0x1000
#define RTL_HEAP_ENTRY_COMMITTED    0x4000
#define RTL_HEAP_ENTRY_LFH          0x8000


/* header for heap blocks */

#define ALIGNMENT (2 * sizeof(void *))

typedef struct block
{
    DWORD  size;                    /* Block size; must be the first field */
    DWORD  magic : 24;              /* Magic number */
    DWORD  unused_bytes : 8;        /* Number of bytes in the block not used by user data (max value is HEAP_MIN_DATA_SIZE+HEAP_MIN_SHRINK_SIZE) */
} ARENA_INUSE;

C_ASSERT( sizeof(struct block) == 8 );

/* block specific flags */

#define BLOCK_FLAG_FREE        0x00000001
#define BLOCK_FLAG_PREV_FREE   0x00000002
#define BLOCK_FLAG_FREE_LINK   0x00000003


/* entry to link free blocks in free lists */

struct DECLSPEC_ALIGN(ALIGNMENT) entry
{
    struct block block;
    struct list entry;
};

C_ASSERT( sizeof(struct entry) == 2 * ALIGNMENT );

typedef struct
{
    struct list           entry;      /* entry in heap large blocks list */
    SIZE_T                data_size;  /* size of user data */
    SIZE_T                block_size; /* total size of virtual memory block */
    DWORD                 pad[2];     /* padding to ensure 16-byte alignment of data */
    DWORD                 size;       /* fields for compatibility with normal arenas */
    DWORD                 magic;      /* these must remain at the end of the structure */
} ARENA_LARGE;

#define ARENA_SIZE_MASK        (~3)
#define ARENA_LARGE_SIZE       0xfedcba90  /* magic value for 'size' field in large blocks */

/* Value for arena 'magic' field */
#define ARENA_INUSE_MAGIC      0x455355
#define ARENA_PENDING_MAGIC    0xbedead
#define ARENA_FREE_MAGIC       0x45455246
#define ARENA_LARGE_MAGIC      0x6752614c

#define ARENA_INUSE_FILLER     0x55
#define ARENA_TAIL_FILLER      0xab
#define ARENA_FREE_FILLER      0xfeeefeee

/* everything is aligned on 8 byte boundaries (16 for Win64) */
#define LARGE_ALIGNMENT        16  /* large blocks have stricter alignment */
#define ARENA_OFFSET           (ALIGNMENT - sizeof(ARENA_INUSE))
#define COMMIT_MASK            0xffff  /* bitmask for commit/decommit granularity */

C_ASSERT( sizeof(ARENA_LARGE) % LARGE_ALIGNMENT == 0 );

#define ROUND_ADDR(addr, mask) ((void *)((UINT_PTR)(addr) & ~(UINT_PTR)(mask)))
#define ROUND_SIZE(size)       ((((size) + ALIGNMENT - 1) & ~(ALIGNMENT-1)) + ARENA_OFFSET)

/* minimum data size (without arenas) of an allocated block */
/* make sure that it's larger than a free list entry */
#define HEAP_MIN_DATA_SIZE    ROUND_SIZE(2 * sizeof(struct list))
#define HEAP_MIN_BLOCK_SIZE   (HEAP_MIN_DATA_SIZE + sizeof(ARENA_INUSE))
/* minimum size that must remain to shrink an allocated block */
#define HEAP_MIN_SHRINK_SIZE  (HEAP_MIN_DATA_SIZE+sizeof(struct entry))
/* minimum size to start allocating large blocks */
#define HEAP_MIN_LARGE_BLOCK_SIZE  (0x10000 * ALIGNMENT - 0x1000)
/* extra size to add at the end of block for tail checking */
#define HEAP_TAIL_EXTRA_SIZE(flags) \
    ((flags & HEAP_TAIL_CHECKING_ENABLED) || RUNNING_ON_VALGRIND ? ALIGNMENT : 0)

/* There will be a free list bucket for every arena size up to and including this value */
#define HEAP_MAX_SMALL_FREE_LIST 0x100
C_ASSERT( HEAP_MAX_SMALL_FREE_LIST % ALIGNMENT == 0 );
#define HEAP_NB_SMALL_FREE_LISTS (((HEAP_MAX_SMALL_FREE_LIST - HEAP_MIN_BLOCK_SIZE) / ALIGNMENT) + 1)

/* Max size of the blocks on the free lists above HEAP_MAX_SMALL_FREE_LIST */
static const SIZE_T free_list_sizes[] =
{
    0x200, 0x400, 0x1000, ~(SIZE_T)0
};
#define HEAP_NB_FREE_LISTS (ARRAY_SIZE(free_list_sizes) + HEAP_NB_SMALL_FREE_LISTS)

struct tagHEAP;

typedef struct DECLSPEC_ALIGN(ALIGNMENT) tagSUBHEAP
{
    SIZE_T __pad[sizeof(SIZE_T) / sizeof(DWORD)];
    SIZE_T              size;       /* Size of the whole sub-heap */
    SIZE_T              commitSize; /* Committed size of the sub-heap */
    struct list         entry;      /* Entry in sub-heap list */
    struct tagHEAP     *heap;       /* Main heap structure */
    struct block block;
} SUBHEAP;

/* block must be last and aligned */
C_ASSERT( sizeof(SUBHEAP) == offsetof(SUBHEAP, block) + sizeof(struct block) );
C_ASSERT( sizeof(SUBHEAP) == 4 * ALIGNMENT );

typedef struct tagHEAP
{                                  /* win32/win64 */
    DWORD_PTR        unknown1[2];   /* 0000/0000 */
    DWORD            ffeeffee;      /* 0008/0010 */
    DWORD            auto_flags;    /* 000c/0014 */
    DWORD_PTR        unknown2[7];   /* 0010/0018 */
    DWORD            unknown3[2];   /* 002c/0050 */
    DWORD_PTR        unknown4[3];   /* 0034/0058 */
    DWORD            flags;         /* 0040/0070 */
    DWORD            force_flags;   /* 0044/0074 */
    /* end of the Windows 10 compatible struct layout */

    BOOL             shared;        /* System shared heap */
    struct list      entry;         /* Entry in process heap list */
    struct list      subheap_list;  /* Sub-heap list */
    struct list      large_list;    /* Large blocks list */
    SIZE_T           grow_size;     /* Size of next subheap for growing heap */
    SIZE_T           min_size;      /* Minimum committed size */
    DWORD            magic;         /* Magic number */
    DWORD            pending_pos;   /* Position in pending free requests ring */
    ARENA_INUSE    **pending_free;  /* Ring buffer for pending free requests */
    RTL_CRITICAL_SECTION cs;
    struct entry     free_lists[HEAP_NB_FREE_LISTS];
    SUBHEAP          subheap;
} HEAP;

/* subheap must be last and aligned */
C_ASSERT( sizeof(HEAP) == offsetof(HEAP, subheap) + sizeof(SUBHEAP) );
C_ASSERT( sizeof(HEAP) % ALIGNMENT == 0 );
C_ASSERT( offsetof(HEAP, subheap) <= COMMIT_MASK );

#define HEAP_MAGIC       ((DWORD)('H' | ('E'<<8) | ('A'<<16) | ('P'<<24)))

#define HEAP_DEF_SIZE        0x110000   /* Default heap size = 1Mb + 64Kb */
#define MAX_FREE_PENDING     1024    /* max number of free requests to delay */

/* some undocumented flags (names are made up) */
#define HEAP_PRIVATE          0x00001000
#define HEAP_PAGE_ALLOCS      0x01000000
#define HEAP_VALIDATE         0x10000000
#define HEAP_VALIDATE_ALL     0x20000000
#define HEAP_VALIDATE_PARAMS  0x40000000

static HEAP *processHeap;  /* main process heap */

/* check if memory range a contains memory range b */
static inline BOOL contains( const void *a, SIZE_T a_size, const void *b, SIZE_T b_size )
{
    const void *a_end = (char *)a + a_size, *b_end = (char *)b + b_size;
    return a <= b && b <= b_end && b_end <= a_end;
}

static inline UINT block_get_flags( const struct block *block )
{
    return block->size & ~ARENA_SIZE_MASK;
}

static inline UINT block_get_type( const struct block *block )
{
    if (block_get_flags( block ) & BLOCK_FLAG_FREE) return (block->unused_bytes << 24)|block->magic;
    return block->magic;
}

static inline void block_set_type( struct block *block, UINT type )
{
    if (type >> 24) block->unused_bytes = type >> 24;
    block->magic = type;
}

static inline UINT block_get_overhead( const struct block *block )
{
    if (block_get_flags( block ) & BLOCK_FLAG_FREE) return sizeof(*block) + sizeof(struct list);
    return sizeof(*block) + block->unused_bytes;
}

/* return the size of a block, including its header */
static inline UINT block_get_size( const struct block *block )
{
    return block->size & ARENA_SIZE_MASK;
}

static inline void block_set_size( struct block *block, UINT flags, UINT block_size )
{
    block->size = (block_size & ARENA_SIZE_MASK) | (flags & ~ARENA_SIZE_MASK);
}

static inline void *subheap_base( const SUBHEAP *subheap )
{
    return ROUND_ADDR( subheap, COMMIT_MASK );
}

static inline SIZE_T subheap_size( const SUBHEAP *subheap )
{
    return subheap->size;
}

static inline SIZE_T subheap_overhead( const SUBHEAP *subheap )
{
    return (char *)&subheap->block - (char *)subheap_base( subheap );
}

static inline const void *subheap_commit_end( const SUBHEAP *subheap )
{
    return (char *)subheap_base( subheap ) + subheap->commitSize;
}

static inline void *first_block( const SUBHEAP *subheap )
{
    return (void *)&subheap->block;
}

static inline const void *last_block( const SUBHEAP *subheap )
{
    return (char *)subheap_commit_end( subheap ) - sizeof(struct block);
}

static inline struct block *next_block( const SUBHEAP *subheap, const struct block *block )
{
    const char *data = (char *)(block + 1), *next, *last = last_block( subheap );
    next = (char *)block + block_get_size( block );
    if (!contains( data, last - (char *)data, next, sizeof(*block) )) return NULL;
    return (struct block *)next;
}

static inline BOOL check_subheap( const SUBHEAP *subheap )
{
    const char *base = ROUND_ADDR( subheap, COMMIT_MASK );
    return contains( base, subheap->size, &subheap->block, base + subheap->commitSize - (char *)&subheap->block );
}

static BOOL heap_validate( const HEAP *heap );

/* mark a block of memory as free for debugging purposes */
static inline void mark_block_free( void *ptr, SIZE_T size, DWORD flags )
{
    if (flags & HEAP_FREE_CHECKING_ENABLED)
    {
        SIZE_T i;
        for (i = 0; i < size / sizeof(DWORD); i++) ((DWORD *)ptr)[i] = ARENA_FREE_FILLER;
    }
#if defined(VALGRIND_MAKE_MEM_NOACCESS)
    VALGRIND_DISCARD( VALGRIND_MAKE_MEM_NOACCESS( ptr, size ));
#elif defined( VALGRIND_MAKE_NOACCESS)
    VALGRIND_DISCARD( VALGRIND_MAKE_NOACCESS( ptr, size ));
#endif
}

/* mark a block of memory as initialized for debugging purposes */
static inline void mark_block_initialized( void *ptr, SIZE_T size )
{
#if defined(VALGRIND_MAKE_MEM_DEFINED)
    VALGRIND_DISCARD( VALGRIND_MAKE_MEM_DEFINED( ptr, size ));
#elif defined(VALGRIND_MAKE_READABLE)
    VALGRIND_DISCARD( VALGRIND_MAKE_READABLE( ptr, size ));
#endif
}

/* mark a block of memory as uninitialized for debugging purposes */
static inline void mark_block_uninitialized( void *ptr, SIZE_T size )
{
#if defined(VALGRIND_MAKE_MEM_UNDEFINED)
    VALGRIND_DISCARD( VALGRIND_MAKE_MEM_UNDEFINED( ptr, size ));
#elif defined(VALGRIND_MAKE_WRITABLE)
    VALGRIND_DISCARD( VALGRIND_MAKE_WRITABLE( ptr, size ));
#endif
}

/* mark a block of memory as a tail block */
static inline void mark_block_tail( void *ptr, SIZE_T size, DWORD flags )
{
    if (flags & HEAP_TAIL_CHECKING_ENABLED)
    {
        mark_block_uninitialized( ptr, size );
        memset( ptr, ARENA_TAIL_FILLER, size );
    }
#if defined(VALGRIND_MAKE_MEM_NOACCESS)
    VALGRIND_DISCARD( VALGRIND_MAKE_MEM_NOACCESS( ptr, size ));
#elif defined( VALGRIND_MAKE_NOACCESS)
    VALGRIND_DISCARD( VALGRIND_MAKE_NOACCESS( ptr, size ));
#endif
}

/* initialize contents of a newly created block of memory */
static inline void initialize_block( void *ptr, SIZE_T size, SIZE_T unused, DWORD flags )
{
    if (flags & HEAP_ZERO_MEMORY)
    {
        mark_block_initialized( ptr, size );
        memset( ptr, 0, size );
    }
    else
    {
        mark_block_uninitialized( ptr, size );
        if (flags & HEAP_FREE_CHECKING_ENABLED)
        {
            memset( ptr, ARENA_INUSE_FILLER, size );
            mark_block_uninitialized( ptr, size );
        }
    }

    mark_block_tail( (char *)ptr + size, unused, flags );
}

/* notify that a new block of memory has been allocated for debugging purposes */
static inline void notify_alloc( void *ptr, SIZE_T size, BOOL init )
{
#ifdef VALGRIND_MALLOCLIKE_BLOCK
    VALGRIND_MALLOCLIKE_BLOCK( ptr, size, 0, init );
#endif
}

/* notify that a block of memory has been freed for debugging purposes */
static inline void notify_free( void const *ptr )
{
#ifdef VALGRIND_FREELIKE_BLOCK
    VALGRIND_FREELIKE_BLOCK( ptr, 0 );
#endif
}

static inline void notify_realloc( void const *ptr, SIZE_T size_old, SIZE_T size_new )
{
#ifdef VALGRIND_RESIZEINPLACE_BLOCK
    /* zero is not a valid size */
    VALGRIND_RESIZEINPLACE_BLOCK( ptr, size_old ? size_old : 1, size_new ? size_new : 1, 0 );
#endif
}

static void notify_free_all( SUBHEAP *subheap )
{
#ifdef VALGRIND_FREELIKE_BLOCK
    struct block *block;

    if (!RUNNING_ON_VALGRIND) return;
    if (!check_subheap( subheap )) return;

    for (block = first_block( subheap ); block; block = next_block( subheap, block ))
    {
        if (block_get_flags( block ) & BLOCK_FLAG_FREE) continue;
        if (block_get_type( block ) == ARENA_INUSE_MAGIC) notify_free( block + 1 );
    }
#endif
}

/* locate a free list entry of the appropriate size */
/* size is the size of the whole block including the arena header */
static inline struct entry *find_free_list( HEAP *heap, SIZE_T block_size, BOOL last )
{
    struct entry *list, *end = heap->free_lists + ARRAY_SIZE(heap->free_lists);
    unsigned int i;

    if (block_size <= HEAP_MAX_SMALL_FREE_LIST)
        i = (block_size - HEAP_MIN_BLOCK_SIZE) / ALIGNMENT;
    else for (i = HEAP_NB_SMALL_FREE_LISTS; i < HEAP_NB_FREE_LISTS - 1; i++)
        if (block_size <= free_list_sizes[i - HEAP_NB_SMALL_FREE_LISTS]) break;

    list = heap->free_lists + i;
    if (last && ++list == end) list = heap->free_lists;
    return list;
}

/* get the memory protection type to use for a given heap */
static inline ULONG get_protection_type( DWORD flags )
{
    return (flags & HEAP_CREATE_ENABLE_EXECUTE) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
}

static RTL_CRITICAL_SECTION_DEBUG process_heap_cs_debug =
{
    0, 0, NULL,  /* will be set later */
    { &process_heap_cs_debug.ProcessLocksList, &process_heap_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": main process heap section") }
};

static inline ULONG heap_get_flags( const HEAP *heap, ULONG flags )
{
    flags &= HEAP_GENERATE_EXCEPTIONS | HEAP_NO_SERIALIZE | HEAP_ZERO_MEMORY | HEAP_REALLOC_IN_PLACE_ONLY;
    return heap->flags | flags;
}

static void heap_lock( HEAP *heap, ULONG flags )
{
    if (heap_get_flags( heap, flags ) & HEAP_NO_SERIALIZE) return;
    RtlEnterCriticalSection( &heap->cs );
}

static void heap_unlock( HEAP *heap, ULONG flags )
{
    if (heap_get_flags( heap, flags ) & HEAP_NO_SERIALIZE) return;
    RtlLeaveCriticalSection( &heap->cs );
}

static void heap_set_status( const HEAP *heap, ULONG flags, NTSTATUS status )
{
    if (status == STATUS_NO_MEMORY && (flags & HEAP_GENERATE_EXCEPTIONS)) RtlRaiseStatus( status );
    if (status) RtlSetLastWin32ErrorAndNtStatusFromNtStatus( status );
}

static void heap_dump( const HEAP *heap )
{
    const struct block *block;
    const ARENA_LARGE *large;
    const SUBHEAP *subheap;
    unsigned int i;
    SIZE_T size;

    TRACE( "heap: %p\n", heap );
    TRACE( "  next %p\n", LIST_ENTRY( heap->entry.next, HEAP, entry ) );

    TRACE( "  free_lists: %p\n", heap->free_lists );
    for (i = 0; i < HEAP_NB_FREE_LISTS; i++)
    {
        if (i < HEAP_NB_SMALL_FREE_LISTS) size = HEAP_MIN_BLOCK_SIZE + i * ALIGNMENT;
        else size = free_list_sizes[i - HEAP_NB_SMALL_FREE_LISTS];
        TRACE( "    %p: size %8Ix, prev %p, next %p\n", heap->free_lists + i, size,
               LIST_ENTRY( heap->free_lists[i].entry.prev, struct entry, entry ),
               LIST_ENTRY( heap->free_lists[i].entry.next, struct entry, entry ) );
    }

    TRACE( "  subheaps: %p\n", &heap->subheap_list );
    LIST_FOR_EACH_ENTRY( subheap, &heap->subheap_list, SUBHEAP, entry )
    {
        SIZE_T free_size = 0, used_size = 0, overhead = 0;
        const char *base = subheap_base( subheap );

        TRACE( "    %p: base %p first %p last %p end %p\n", subheap, base, first_block( subheap ),
               last_block( subheap ), base + subheap_size( subheap ) );

        if (!check_subheap( subheap )) return;

        overhead += subheap_overhead( subheap );
        for (block = first_block( subheap ); block; block = next_block( subheap, block ))
        {
            if (block_get_flags( block ) & BLOCK_FLAG_FREE)
            {
                TRACE( "      %p: (free) type %#10x, size %#8x, flags %#4x, prev %p, next %p\n", block,
                       block_get_type( block ), block_get_size( block ), block_get_flags( block ),
                       LIST_ENTRY( ((struct entry *)block)->entry.prev, struct entry, entry ),
                       LIST_ENTRY( ((struct entry *)block)->entry.next, struct entry, entry ) );

                overhead += block_get_overhead( block );
                free_size += block_get_size( block ) - block_get_overhead( block );
            }
            else
            {
                TRACE( "      %p: (used) type %#10x, size %#8x, flags %#4x, unused %#4x", block,
                       block_get_type( block ), block_get_size( block ), block_get_flags( block ),
                       block->unused_bytes );
                if (!(block_get_flags( block ) & BLOCK_FLAG_PREV_FREE)) TRACE( "\n" );
                else TRACE( ", back %p\n", *((struct block **)block - 1) );

                overhead += block_get_overhead( block );
                used_size += block_get_size( block ) - block_get_overhead( block );
            }
        }

        TRACE( "      total %#Ix, used %#Ix, free %#Ix, overhead %#Ix (%Iu%%)\n", used_size + free_size + overhead,
               used_size, free_size, overhead, (overhead * 100) / subheap_size( subheap ) );
    }

    TRACE( "  large blocks: %p\n", &heap->large_list );
    LIST_FOR_EACH_ENTRY( large, &heap->large_list, ARENA_LARGE, entry )
    {
        block = (struct block *)(large + 1) - 1;
        TRACE( "    %p: (large) type %#10x, size %#8x, flags %#4x, total_size %#10Ix, alloc_size %#10Ix, prev %p, next %p\n",
               large, block_get_type( block ), block_get_size( block ), block_get_flags( block ), large->block_size, large->data_size,
               LIST_ENTRY( large->entry.prev, ARENA_LARGE, entry ), LIST_ENTRY( large->entry.next, ARENA_LARGE, entry ) );
    }

    if (heap->pending_free)
    {
        TRACE( "  pending blocks: %p\n", heap->pending_free );
        for (i = 0; i < MAX_FREE_PENDING; ++i)
        {
            if (!(block = heap->pending_free[i])) break;

            TRACE( "   %c%p: (pend) type %#10x, size %#8x, flags %#4x, unused %#4x", i == heap->pending_pos ? '*' : ' ',
                   block, block_get_type( block ), block_get_size( block ), block_get_flags( block ), block->unused_bytes );
            if (!(block_get_flags( block ) & BLOCK_FLAG_PREV_FREE)) TRACE( "\n" );
            else TRACE( ", back %p\n", *((struct block **)block - 1) );
        }
    }
}

static const char *debugstr_heap_entry( struct rtl_heap_entry *entry )
{
    const char *str = wine_dbg_sprintf( "data %p, size %#Ix, overhead %#x, region %#x, flags %#x", entry->lpData,
                                        entry->cbData, entry->cbOverhead, entry->iRegionIndex, entry->wFlags );
    if (!(entry->wFlags & RTL_HEAP_ENTRY_REGION)) return str;
    return wine_dbg_sprintf( "%s, commit %#x, uncommit %#x, first %p, last %p", str, entry->Region.dwCommittedSize,
                             entry->Region.dwUnCommittedSize, entry->Region.lpFirstBlock, entry->Region.lpLastBlock );
}

/***********************************************************************
 *           HEAP_GetPtr
 * RETURNS
 *	Pointer to the heap
 *	NULL: Failure
 */
static HEAP *HEAP_GetPtr(
             HANDLE heap /* [in] Handle to the heap */
) {
    HEAP *heapPtr = heap;
    BOOL valid = TRUE;

    if (!heapPtr || (heapPtr->magic != HEAP_MAGIC))
    {
        ERR("Invalid heap %p!\n", heap );
        return NULL;
    }
    if (heapPtr->flags & HEAP_VALIDATE_ALL)
    {
        heap_lock( heapPtr, 0 );
        valid = heap_validate( heapPtr );
        heap_unlock( heapPtr, 0 );

        if (!valid && TRACE_ON(heap))
        {
            heap_dump( heapPtr );
            assert( FALSE );
        }
    }

    return valid ? heapPtr : NULL;
}


static SUBHEAP *find_subheap( const HEAP *heap, const struct block *block, BOOL heap_walk )
{
    SUBHEAP *subheap;

    LIST_FOR_EACH_ENTRY( subheap, &heap->subheap_list, SUBHEAP, entry )
    {
        SIZE_T blocks_size = (char *)last_block( subheap ) - (char *)first_block( subheap );
        if (!check_subheap( subheap )) return NULL;
        if (contains( first_block( subheap ), blocks_size, block, sizeof(*block) )) return subheap;
        /* outside of blocks region, possible corruption or heap_walk */
        if (contains( subheap_base( subheap ), subheap_size( subheap ), block, 0 )) return heap_walk ? subheap : NULL;
    }

    return NULL;
}


static inline BOOL subheap_commit( SUBHEAP *subheap, const struct block *block, SIZE_T block_size )
{
    const char *end = (char *)subheap_base( subheap ) + subheap_size( subheap ), *commit_end;
    HEAP *heap = subheap->heap;
    ULONG flags = heap->flags;
    SIZE_T size;
    void *addr;

    commit_end = (char *)block + block_size + sizeof(struct entry);
    commit_end = ROUND_ADDR((char *)commit_end + COMMIT_MASK, COMMIT_MASK);

    if (commit_end > end) commit_end = end;
    if (commit_end <= (char *)subheap_commit_end( subheap )) return TRUE;

    addr = (void *)subheap_commit_end( subheap );
    size = commit_end - (char *)addr;

    if (NtAllocateVirtualMemory( NtCurrentProcess(), &addr, 0, &size, MEM_COMMIT,
                                 get_protection_type( flags ) ))
    {
        WARN( "Could not commit %#Ix bytes at %p for heap %p\n", size, addr, heap );
        return FALSE;
    }

    subheap->commitSize = (char *)commit_end - (char *)subheap_base( subheap );
    return TRUE;
}

static inline BOOL subheap_decommit( SUBHEAP *subheap, const void *commit_end )
{
    char *base = subheap_base( subheap );
    HEAP *heap = subheap->heap;
    SIZE_T size;
    void *addr;

    commit_end = ROUND_ADDR((char *)commit_end + COMMIT_MASK, COMMIT_MASK);
    if (subheap == &heap->subheap) commit_end = max( (char *)commit_end, (char *)base + heap->min_size );
    if (commit_end >= subheap_commit_end( subheap )) return TRUE;

    size = (char *)subheap_commit_end( subheap ) - (char *)commit_end;
    addr = (void *)commit_end;

    if (NtFreeVirtualMemory( NtCurrentProcess(), &addr, &size, MEM_DECOMMIT ))
    {
        WARN( "Could not decommit %#Ix bytes at %p for heap %p\n", size, addr, heap );
        return FALSE;
    }

    subheap->commitSize = (char *)commit_end - (char *)subheap_base( subheap );
    return TRUE;
}


static void create_free_block( SUBHEAP *subheap, struct block *block, SIZE_T block_size )
{
    const char *end = (char *)block + block_size, *commit_end = subheap_commit_end( subheap );
    struct entry *entry = (struct entry *)block, *list;
    HEAP *heap = subheap->heap;
    DWORD flags = heap->flags;
    struct block *next;

    mark_block_uninitialized( block, sizeof(*entry) );
    block_set_type( block, ARENA_FREE_MAGIC );
    block_set_size( block, BLOCK_FLAG_FREE, block_size );

    /* If debugging, erase the freed block content */

    if (end > commit_end) end = commit_end;
    if (end > (char *)(entry + 1)) mark_block_free( entry + 1, end - (char *)(entry + 1), flags );

    if ((next = next_block( subheap, block )) && (block_get_flags( next ) & BLOCK_FLAG_FREE))
    {
        /* merge with the next block if it is free */
        struct entry *next_entry = (struct entry *)next;
        list_remove( &next_entry->entry );
        block_size += block_get_size( next );
        block_set_size( block, BLOCK_FLAG_FREE, block_size );
        mark_block_free( next_entry, sizeof(*next_entry), flags );
    }

    if ((next = next_block( subheap, block )))
    {
        /* set the next block PREV_FREE flag and back pointer */
        block_set_size( next, BLOCK_FLAG_PREV_FREE, block_get_size( next ) );
        mark_block_initialized( (struct block **)next - 1, sizeof(struct block *) );
        *((struct block **)next - 1) = block;
    }

    list = find_free_list( heap, block_get_size( block ), !next );
    if (!next) list_add_before( &list->entry, &entry->entry );
    else list_add_after( &list->entry, &entry->entry );
}


static void free_used_block( SUBHEAP *subheap, struct block *block )
{
    HEAP *heap = subheap->heap;
    struct entry *entry;
    SIZE_T block_size;

    if (heap->pending_free)
    {
        struct block *tmp = heap->pending_free[heap->pending_pos];
        heap->pending_free[heap->pending_pos] = block;
        heap->pending_pos = (heap->pending_pos + 1) % MAX_FREE_PENDING;
        block_set_type( block, ARENA_PENDING_MAGIC );
        mark_block_free( block + 1, block_get_size( block ) - sizeof(*block), heap->flags );
        if (!(block = tmp) || !(subheap = find_subheap( heap, block, FALSE ))) return;
    }

    block_size = block_get_size( block );
    if (block_get_flags( block ) & BLOCK_FLAG_PREV_FREE)
    {
        /* merge with previous block if it is free */
        block = *((struct block **)block - 1);
        block_size += block_get_size( block );
        entry = (struct entry *)block;
        list_remove( &entry->entry );
    }
    else entry = (struct entry *)block;

    create_free_block( subheap, block, block_size );
    if (next_block( subheap, block )) return;  /* not the last block */

    if (block == first_block( subheap ) && subheap != &heap->subheap)
    {
        /* free the subheap if it's empty and not the main one */
        void *addr = subheap_base( subheap );
        SIZE_T size = 0;

        list_remove( &entry->entry );
        list_remove( &subheap->entry );
        NtFreeVirtualMemory( NtCurrentProcess(), &addr, &size, MEM_RELEASE );
    }
    else if (!heap->shared)
    {
        /* keep room for a full commited block as hysteresis */
        subheap_decommit( subheap, (char *)(entry + 1) + (COMMIT_MASK + 1) );
    }
}


static inline void shrink_used_block( SUBHEAP *subheap, struct block *block, UINT flags,
                                      SIZE_T old_block_size, SIZE_T block_size, SIZE_T size )
{
    if (old_block_size >= block_size + HEAP_MIN_SHRINK_SIZE)
    {
        block_set_size( block, flags, block_size );
        block->unused_bytes = block_size - sizeof(*block) - size;
        create_free_block( subheap, next_block( subheap, block ), old_block_size - block_size );
    }
    else
    {
        struct block *next;
        block_set_size( block, flags, old_block_size );
        block->unused_bytes = old_block_size - sizeof(*block) - size;
        if ((next = next_block( subheap, block ))) next->size &= ~BLOCK_FLAG_PREV_FREE;
    }
}


/***********************************************************************
 *           allocate_large_block
 */
static void *allocate_large_block( HEAP *heap, DWORD flags, SIZE_T size )
{
    ARENA_LARGE *arena;
    SIZE_T block_size = sizeof(*arena) + ROUND_SIZE(size) + HEAP_TAIL_EXTRA_SIZE(flags);
    LPVOID address = NULL;

    if (!(flags & HEAP_GROWABLE)) return NULL;
    if (block_size < size) return NULL;  /* overflow */
    if (NtAllocateVirtualMemory( NtCurrentProcess(), &address, 0, &block_size,
                                 MEM_COMMIT, get_protection_type( flags )))
    {
        WARN("Could not allocate block for %08lx bytes\n", size );
        return NULL;
    }
    arena = address;
    arena->data_size = size;
    arena->block_size = block_size;
    arena->size = ARENA_LARGE_SIZE;
    arena->magic = ARENA_LARGE_MAGIC;
    mark_block_tail( (char *)(arena + 1) + size, block_size - sizeof(*arena) - size, flags );
    list_add_tail( &heap->large_list, &arena->entry );
    notify_alloc( arena + 1, size, flags & HEAP_ZERO_MEMORY );
    return arena + 1;
}


/***********************************************************************
 *           free_large_block
 */
static void free_large_block( HEAP *heap, void *ptr )
{
    ARENA_LARGE *arena = (ARENA_LARGE *)ptr - 1;
    LPVOID address = arena;
    SIZE_T size = 0;

    list_remove( &arena->entry );
    NtFreeVirtualMemory( NtCurrentProcess(), &address, &size, MEM_RELEASE );
}


/***********************************************************************
 *           realloc_large_block
 */
static void *realloc_large_block( HEAP *heap, DWORD flags, void *ptr, SIZE_T size )
{
    ARENA_LARGE *arena = (ARENA_LARGE *)ptr - 1;
    void *new_ptr;

    if (arena->block_size - sizeof(*arena) >= size)
    {
        SIZE_T unused = arena->block_size - sizeof(*arena) - size;

        /* FIXME: we could remap zero-pages instead */
#ifdef VALGRIND_RESIZEINPLACE_BLOCK
        if (RUNNING_ON_VALGRIND)
            notify_realloc( arena + 1, arena->data_size, size );
        else
#endif
        if (size > arena->data_size)
            initialize_block( (char *)ptr + arena->data_size, size - arena->data_size, unused, flags );
        else
            mark_block_tail( (char *)ptr + size, unused, flags );
        arena->data_size = size;
        return ptr;
    }
    if (flags & HEAP_REALLOC_IN_PLACE_ONLY) return NULL;
    if (!(new_ptr = allocate_large_block( heap, flags, size )))
    {
        WARN("Could not allocate block for %08lx bytes\n", size );
        return NULL;
    }
    memcpy( new_ptr, ptr, arena->data_size );
    free_large_block( heap, ptr );
    notify_free( ptr );
    return new_ptr;
}


/***********************************************************************
 *           find_large_block
 */
static ARENA_LARGE *find_large_block( const HEAP *heap, const void *ptr )
{
    ARENA_LARGE *arena;

    LIST_FOR_EACH_ENTRY( arena, &heap->large_list, ARENA_LARGE, entry )
        if (ptr == arena + 1) return arena;

    return NULL;
}

static BOOL validate_large_arena( const HEAP *heap, const ARENA_LARGE *arena )
{
    const char *err = NULL;

    if ((ULONG_PTR)arena & COMMIT_MASK)
        err = "invalid block alignment";
    else if (arena->size != ARENA_LARGE_SIZE || arena->magic != ARENA_LARGE_MAGIC)
        err = "invalid block header";
    else if (!contains( arena, arena->block_size, arena + 1, arena->data_size ))
        err = "invalid block size";
    else if (heap->flags & HEAP_TAIL_CHECKING_ENABLED)
    {
        SIZE_T i, unused = arena->block_size - sizeof(*arena) - arena->data_size;
        const unsigned char *data = (const unsigned char *)(arena + 1) + arena->data_size;
        for (i = 0; i < unused && !err; i++) if (data[i] != ARENA_TAIL_FILLER) err = "invalid block tail";
    }

    if (err)
    {
        ERR( "heap %p, block %p: %s\n", heap, arena, err );
        if (TRACE_ON(heap)) heap_dump( heap );
    }

    return !err;
}

/***********************************************************************
 *           HEAP_CreateSubHeap
 */
static SUBHEAP *HEAP_CreateSubHeap( HEAP *heap, LPVOID address, DWORD flags,
                                    SIZE_T commitSize, SIZE_T totalSize )
{
    struct entry *pEntry;
    SUBHEAP *subheap;
    unsigned int i;

    if (!address)
    {
        if (!commitSize) commitSize = COMMIT_MASK + 1;
        totalSize = min( totalSize, 0xffff0000 );  /* don't allow a heap larger than 4GB */
        if (totalSize < commitSize) totalSize = commitSize;
        if (flags & HEAP_SHARED) commitSize = totalSize;  /* always commit everything in a shared heap */
        commitSize = min( totalSize, (commitSize + COMMIT_MASK) & ~COMMIT_MASK );

        /* allocate the memory block */
        if (NtAllocateVirtualMemory( NtCurrentProcess(), &address, 0, &totalSize,
                                     MEM_RESERVE, get_protection_type( flags ) ))
        {
            WARN("Could not allocate %08lx bytes\n", totalSize );
            return NULL;
        }
        if (NtAllocateVirtualMemory( NtCurrentProcess(), &address, 0,
                                     &commitSize, MEM_COMMIT, get_protection_type( flags ) ))
        {
            WARN("Could not commit %08lx bytes for sub-heap %p\n", commitSize, address );
            return NULL;
        }
    }

    if (heap)
    {
        /* If this is a secondary subheap, insert it into list */

        subheap = address;
        subheap->heap       = heap;
        subheap->size       = totalSize;
        subheap->commitSize = commitSize;
        list_add_head( &heap->subheap_list, &subheap->entry );
    }
    else
    {
        /* If this is a primary subheap, initialize main heap */

        heap = address;
        heap->ffeeffee      = 0xffeeffee;
        heap->auto_flags    = (flags & HEAP_GROWABLE);
        heap->flags         = (flags & ~HEAP_SHARED);
        heap->shared        = (flags & HEAP_SHARED) != 0;
        heap->magic         = HEAP_MAGIC;
        heap->grow_size     = max( HEAP_DEF_SIZE, totalSize );
        heap->min_size      = commitSize;
        list_init( &heap->subheap_list );
        list_init( &heap->large_list );

        subheap = &heap->subheap;
        subheap->heap       = heap;
        subheap->size       = totalSize;
        subheap->commitSize = commitSize;
        list_add_head( &heap->subheap_list, &subheap->entry );

        /* Build the free lists */

        list_init( &heap->free_lists[0].entry );
        for (i = 0, pEntry = heap->free_lists; i < HEAP_NB_FREE_LISTS; i++, pEntry++)
        {
            block_set_size( &pEntry->block, BLOCK_FLAG_FREE_LINK, 0 );
            block_set_type( &pEntry->block, ARENA_FREE_MAGIC );
            if (i) list_add_after( &pEntry[-1].entry, &pEntry->entry );
        }

        /* Initialize critical section */

        if (!processHeap)  /* do it by hand to avoid memory allocations */
        {
            heap->cs.DebugInfo      = &process_heap_cs_debug;
            heap->cs.LockCount      = -1;
            heap->cs.RecursionCount = 0;
            heap->cs.OwningThread   = 0;
            heap->cs.LockSemaphore  = 0;
            heap->cs.SpinCount      = 0;
            process_heap_cs_debug.CriticalSection = &heap->cs;
        }
        else
        {
            RtlInitializeCriticalSection( &heap->cs );
            heap->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": heap.cs");
        }

        if (heap->shared)
        {
            /* let's assume that only one thread at a time will try to do this */
            HANDLE sem = heap->cs.LockSemaphore;
            if (!sem) NtCreateSemaphore( &sem, SEMAPHORE_ALL_ACCESS, NULL, 0, 1 );

            NtDuplicateObject( NtCurrentProcess(), sem, NtCurrentProcess(), &sem, 0, 0,
                               DUPLICATE_MAKE_GLOBAL | DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE );
            heap->cs.LockSemaphore = sem;
            RtlFreeHeap( processHeap, 0, heap->cs.DebugInfo );
            heap->cs.DebugInfo = NULL;
        }
    }

    /* Create the first free block */

    create_free_block( subheap, first_block( subheap ), subheap_size( subheap ) - subheap_overhead( subheap ) );

    return subheap;
}


static struct block *find_free_block( HEAP *heap, SIZE_T block_size, SUBHEAP **subheap )
{
    struct list *ptr = &find_free_list( heap, block_size, FALSE )->entry;
    struct entry *entry;
    struct block *block;
    SIZE_T total_size;

    /* Find a suitable free list, and in it find a block large enough */

    while ((ptr = list_next( &heap->free_lists[0].entry, ptr )))
    {
        entry = LIST_ENTRY( ptr, struct entry, entry );
        block = (struct block *)entry;
        if (block_get_flags( block ) == BLOCK_FLAG_FREE_LINK) continue;
        if (block_get_size( block ) >= block_size)
        {
            *subheap = find_subheap( heap, block, FALSE );
            if (!subheap_commit( *subheap, block, block_size )) return NULL;
            list_remove( &entry->entry );
            return block;
        }
    }

    /* If no block was found, attempt to grow the heap */

    if (!(heap->flags & HEAP_GROWABLE))
    {
        WARN("Not enough space in heap %p for %08lx bytes\n", heap, block_size );
        return NULL;
    }

    /* make sure we can fit the block and a free entry at the end */
    total_size = sizeof(SUBHEAP) + block_size + sizeof(struct entry);
    if (total_size < block_size) return NULL;  /* overflow */

    if ((*subheap = HEAP_CreateSubHeap( heap, NULL, heap->flags, total_size,
                                        max( heap->grow_size, total_size ) )))
    {
        if (heap->grow_size < 128 * 1024 * 1024) heap->grow_size *= 2;
    }
    else while (!*subheap)  /* shrink the grow size again if we are running out of space */
    {
        if (heap->grow_size <= total_size || heap->grow_size <= 4 * 1024 * 1024) return NULL;
        heap->grow_size /= 2;
        *subheap = HEAP_CreateSubHeap( heap, NULL, heap->flags, total_size,
                                       max( heap->grow_size, total_size ) );
    }

    TRACE( "created new sub-heap %p of %08lx bytes for heap %p\n", *subheap, subheap_size( *subheap ), heap );

    entry = first_block( *subheap );
    list_remove( &entry->entry );
    return (struct block *)entry;
}


static BOOL is_valid_free_block( const HEAP *heap, const struct block *block )
{
    const SUBHEAP *subheap;
    unsigned int i;

    if ((subheap = find_subheap( heap, block, FALSE ))) return TRUE;
    for (i = 0; i < HEAP_NB_FREE_LISTS; i++) if (block == &heap->free_lists[i].block) return TRUE;
    return FALSE;
}

static BOOL validate_free_block( const SUBHEAP *subheap, const struct block *block )
{
    const char *err = NULL, *base = subheap_base( subheap ), *commit_end = subheap_commit_end( subheap );
    const struct entry *entry = (struct entry *)block;
    const struct block *prev, *next;
    HEAP *heap = subheap->heap;
    DWORD flags = heap->flags;

    if ((ULONG_PTR)(block + 1) % ALIGNMENT)
        err = "invalid block alignment";
    else if (block_get_type( block ) != ARENA_FREE_MAGIC)
        err = "invalid block header";
    else if (!(block_get_flags( block ) & BLOCK_FLAG_FREE) || (block_get_flags( block ) & BLOCK_FLAG_PREV_FREE))
        err = "invalid block flags";
    else if (!contains( base, subheap_size( subheap ), block, block_get_size( block ) ))
        err = "invalid block size";
    else if (!is_valid_free_block( heap, (next = (struct block *)LIST_ENTRY( entry->entry.next, struct entry, entry )) ))
        err = "invalid next free block pointer";
    else if (!(block_get_flags( next ) & BLOCK_FLAG_FREE) || block_get_type( next ) != ARENA_FREE_MAGIC)
        err = "invalid next free block header";
    else if (!is_valid_free_block( heap, (prev = (struct block *)LIST_ENTRY( entry->entry.prev, struct entry, entry )) ))
        err = "invalid previous free block pointer";
    else if (!(block_get_flags( prev ) & BLOCK_FLAG_FREE) || block_get_type( prev ) != ARENA_FREE_MAGIC)
        err = "invalid previous free block header";
    else if ((next = next_block( subheap, (struct block *)block )))
    {
        if (!(block_get_flags( next ) & BLOCK_FLAG_PREV_FREE))
            err = "invalid next block flags";
        if (*((struct block **)next - 1) != block)
            err = "invalid next block back pointer";
    }

    if (!err && (flags & HEAP_FREE_CHECKING_ENABLED))
    {
        const char *ptr = (char *)(entry + 1), *end = (char *)block + block_get_size( block );
        if (next) end -= sizeof(struct block *);
        if (end > commit_end) end = commit_end;
        while (!err && ptr < end)
        {
            if (*(DWORD *)ptr != ARENA_FREE_FILLER) err = "free block overwritten";
            ptr += sizeof(DWORD);
        }
    }

    if (err)
    {
        ERR( "heap %p, block %p: %s\n", heap, block, err );
        if (TRACE_ON(heap)) heap_dump( heap );
    }

    return !err;
}


static BOOL validate_used_block( const SUBHEAP *subheap, const struct block *block )
{
    const char *err = NULL, *base = subheap_base( subheap ), *commit_end = subheap_commit_end( subheap );
    const HEAP *heap = subheap->heap;
    DWORD flags = heap->flags;
    const struct block *next;
    int i;

    if ((ULONG_PTR)(block + 1) % ALIGNMENT)
        err = "invalid block alignment";
    else if (block_get_type( block ) != ARENA_INUSE_MAGIC && block_get_type( block ) != ARENA_PENDING_MAGIC)
        err = "invalid block header";
    else if (block_get_flags( block ) & BLOCK_FLAG_FREE)
        err = "invalid block flags";
    else if (!contains( base, commit_end - base, block, block_get_size( block ) ))
        err = "invalid block size";
    else if (block->unused_bytes > block_get_size( block ) - sizeof(*block))
        err = "invalid block unused size";
    else if ((next = next_block( subheap, block )) && (block_get_flags( next ) & BLOCK_FLAG_PREV_FREE))
        err = "invalid next block flags";
    else if (block_get_flags( block ) & BLOCK_FLAG_PREV_FREE)
    {
        const struct block *prev = *((struct block **)block - 1);
        if (!is_valid_free_block( heap, prev ))
            err = "invalid previous block pointer";
        else if (!(block_get_flags( prev ) & BLOCK_FLAG_FREE) || block_get_type( prev ) != ARENA_FREE_MAGIC)
            err = "invalid previous block flags";
        if ((char *)prev + block_get_size( prev ) != (char *)block)
            err = "invalid previous block size";
    }

    if (!err && block_get_type( block ) == ARENA_PENDING_MAGIC)
    {
        const char *ptr = (char *)(block + 1), *end = (char *)block + block_get_size( block );
        while (!err && ptr < end)
        {
            if (*(DWORD *)ptr != ARENA_FREE_FILLER) err = "free block overwritten";
            ptr += sizeof(DWORD);
        }
    }
    else if (!err && (flags & HEAP_TAIL_CHECKING_ENABLED))
    {
        const unsigned char *tail = (unsigned char *)block + block_get_size( block ) - block->unused_bytes;
        for (i = 0; !err && i < block->unused_bytes; i++) if (tail[i] != ARENA_TAIL_FILLER) err = "invalid block tail";
    }

    if (err)
    {
        ERR( "heap %p, block %p: %s\n", heap, block, err );
        if (TRACE_ON(heap)) heap_dump( heap );
    }

    return !err;
}


static BOOL heap_validate_ptr( const HEAP *heap, const void *ptr, SUBHEAP **subheap )
{
    const struct block *arena = (struct block *)ptr - 1;
    const ARENA_LARGE *large_arena;

    if (!(*subheap = find_subheap( heap, arena, FALSE )))
    {
        if (!(large_arena = find_large_block( heap, ptr )))
        {
            if (WARN_ON(heap)) WARN("heap %p, ptr %p: block region not found\n", heap, ptr );
            return FALSE;
        }

        return validate_large_arena( heap, large_arena );
    }

    return validate_used_block( *subheap, arena );
}

static BOOL heap_validate( const HEAP *heap )
{
    const ARENA_LARGE *large_arena;
    const struct block *block;
    const SUBHEAP *subheap;

    LIST_FOR_EACH_ENTRY( subheap, &heap->subheap_list, SUBHEAP, entry )
    {
        if (!check_subheap( subheap ))
        {
            ERR( "heap %p, subheap %p corrupted sizes\n", heap, subheap );
            if (TRACE_ON(heap)) heap_dump( heap );
            return FALSE;
        }

        for (block = first_block( subheap ); block; block = next_block( subheap, block ))
        {
            if (block_get_flags( block ) & BLOCK_FLAG_FREE)
            {
                if (!validate_free_block( subheap, block )) return FALSE;
            }
            else
            {
                if (!validate_used_block( subheap, block )) return FALSE;
            }
        }
    }

    LIST_FOR_EACH_ENTRY( large_arena, &heap->large_list, ARENA_LARGE, entry )
        if (!validate_large_arena( heap, large_arena )) return FALSE;

    return TRUE;
}

static inline struct block *unsafe_block_from_ptr( const HEAP *heap, const void *ptr, SUBHEAP **subheap )
{
    struct block *block = (struct block *)ptr - 1;
    const char *err = NULL, *base, *commit_end;

    if (heap->flags & HEAP_VALIDATE)
    {
        if (!heap_validate_ptr( heap, ptr, subheap )) return NULL;
        return block;
    }

    if ((*subheap = find_subheap( heap, block, FALSE )))
    {
        base = subheap_base( *subheap );
        commit_end = subheap_commit_end( *subheap );
    }

    if (!*subheap)
    {
        if (find_large_block( heap, ptr )) return block;
        err = "block region not found";
    }
    else if ((ULONG_PTR)ptr % ALIGNMENT)
        err = "invalid ptr alignment";
    else if (block_get_type( block ) == ARENA_PENDING_MAGIC || (block_get_flags( block ) & BLOCK_FLAG_FREE))
        err = "already freed block";
    else if (block_get_type( block ) != ARENA_INUSE_MAGIC)
        err = "invalid block header";
    else if (!contains( base, commit_end - base, block, block_get_size( block ) ))
        err = "invalid block size";

    if (err) WARN( "heap %p, block %p: %s\n", heap, block, err );
    return err ? NULL : block;
}

static DWORD heap_flags_from_global_flag( DWORD flag )
{
    DWORD ret = 0;

    if (flag & FLG_HEAP_ENABLE_TAIL_CHECK)
        ret |= HEAP_TAIL_CHECKING_ENABLED;
    if (flag & FLG_HEAP_ENABLE_FREE_CHECK)
        ret |= HEAP_FREE_CHECKING_ENABLED;
    if (flag & FLG_HEAP_VALIDATE_PARAMETERS)
        ret |= HEAP_VALIDATE_PARAMS | HEAP_TAIL_CHECKING_ENABLED | HEAP_FREE_CHECKING_ENABLED;
    if (flag & FLG_HEAP_VALIDATE_ALL)
        ret |= HEAP_VALIDATE_ALL | HEAP_TAIL_CHECKING_ENABLED | HEAP_FREE_CHECKING_ENABLED;
    if (flag & FLG_HEAP_DISABLE_COALESCING)
        ret |= HEAP_DISABLE_COALESCE_ON_FREE;
    if (flag & FLG_HEAP_PAGE_ALLOCS)
        ret |= HEAP_PAGE_ALLOCS;

    return ret;
}

/***********************************************************************
 *           heap_set_debug_flags
 */
static void heap_set_debug_flags( HANDLE handle )
{
    HEAP *heap = HEAP_GetPtr( handle );
    ULONG global_flags = RtlGetNtGlobalFlags();
    DWORD flags, force_flags;

    if (TRACE_ON(heap)) global_flags |= FLG_HEAP_VALIDATE_ALL;
    if (WARN_ON(heap)) global_flags |= FLG_HEAP_VALIDATE_PARAMETERS;

    flags = heap_flags_from_global_flag( global_flags );
    force_flags = (heap->flags | flags) & ~(HEAP_SHARED|HEAP_DISABLE_COALESCE_ON_FREE);

    if (global_flags & FLG_HEAP_ENABLE_TAGGING) flags |= HEAP_SHARED;
    if (!(global_flags & FLG_HEAP_PAGE_ALLOCS)) force_flags &= ~(HEAP_GROWABLE|HEAP_PRIVATE);

    if (RUNNING_ON_VALGRIND) flags = 0; /* no sense in validating since Valgrind catches accesses */

    heap->flags |= flags;
    heap->force_flags |= force_flags;

    if (flags & (HEAP_FREE_CHECKING_ENABLED | HEAP_TAIL_CHECKING_ENABLED))  /* fix existing blocks */
    {
        struct block *block;
        ARENA_LARGE *large;
        SUBHEAP *subheap;

        LIST_FOR_EACH_ENTRY( subheap, &heap->subheap_list, SUBHEAP, entry )
        {
            const char *commit_end = subheap_commit_end( subheap );

            if (!check_subheap( subheap )) break;

            for (block = first_block( subheap ); block; block = next_block( subheap, block ))
            {
                if (block_get_flags( block ) & BLOCK_FLAG_FREE)
                {
                    char *data = (char *)block + block_get_overhead( block ), *end = (char *)block + block_get_size( block );
                    if (end >= commit_end) mark_block_free( data, commit_end - data, flags );
                    else mark_block_free( data, end - sizeof(struct block *) - data, flags );
                }
                else
                {
                    if (block_get_type( block ) == ARENA_PENDING_MAGIC) mark_block_free( block + 1, block_get_size( block ) - sizeof(*block), flags );
                    else mark_block_tail( (char *)block + block_get_size( block ) - block->unused_bytes, block->unused_bytes, flags );
                }
            }
        }

        LIST_FOR_EACH_ENTRY( large, &heap->large_list, ARENA_LARGE, entry )
            mark_block_tail( (char *)(large + 1) + large->data_size,
                             large->block_size - sizeof(*large) - large->data_size, flags );
    }

    if ((heap->flags & HEAP_GROWABLE) && !heap->pending_free &&
        ((flags & HEAP_FREE_CHECKING_ENABLED) || RUNNING_ON_VALGRIND))
    {
        heap->pending_free = RtlAllocateHeap( handle, HEAP_ZERO_MEMORY,
                                              MAX_FREE_PENDING * sizeof(*heap->pending_free) );
        heap->pending_pos = 0;
    }
}


/***********************************************************************
 *           RtlCreateHeap   (NTDLL.@)
 *
 * Create a new Heap.
 *
 * PARAMS
 *  flags      [I] HEAP_ flags from "winnt.h"
 *  addr       [I] Desired base address
 *  totalSize  [I] Total size of the heap, or 0 for a growable heap
 *  commitSize [I] Amount of heap space to commit
 *  unknown    [I] Not yet understood
 *  definition [I] Heap definition
 *
 * RETURNS
 *  Success: A HANDLE to the newly created heap.
 *  Failure: a NULL HANDLE.
 */
HANDLE WINAPI RtlCreateHeap( ULONG flags, PVOID addr, SIZE_T totalSize, SIZE_T commitSize,
                             PVOID unknown, PRTL_HEAP_DEFINITION definition )
{
    SUBHEAP *subheap;
    HEAP *heap;

    /* Allocate the heap block */

    flags &= ~(HEAP_TAIL_CHECKING_ENABLED|HEAP_FREE_CHECKING_ENABLED);
    if (processHeap) flags |= HEAP_PRIVATE;
    if (!processHeap || !totalSize || (flags & HEAP_SHARED)) flags |= HEAP_GROWABLE;
    if (!totalSize) totalSize = HEAP_DEF_SIZE;

    if (!(subheap = HEAP_CreateSubHeap( NULL, addr, flags, commitSize, totalSize ))) return 0;
    heap = subheap->heap;

    heap_set_debug_flags( heap );

    /* link it into the per-process heap list */
    if (processHeap)
    {
        RtlEnterCriticalSection( &processHeap->cs );
        list_add_head( &processHeap->entry, &heap->entry );
        RtlLeaveCriticalSection( &processHeap->cs );
    }
    else if (!addr)
    {
        processHeap = heap;  /* assume the first heap we create is the process main heap */
        list_init( &processHeap->entry );
    }

    return heap;
}


/***********************************************************************
 *           RtlDestroyHeap   (NTDLL.@)
 *
 * Destroy a Heap created with RtlCreateHeap().
 *
 * PARAMS
 *  heap [I] Heap to destroy.
 *
 * RETURNS
 *  Success: A NULL HANDLE, if heap is NULL or it was destroyed
 *  Failure: The Heap handle, if heap is the process heap.
 */
HANDLE WINAPI RtlDestroyHeap( HANDLE heap )
{
    HEAP *heapPtr = HEAP_GetPtr( heap );
    SUBHEAP *subheap, *next;
    ARENA_LARGE *arena, *arena_next;
    SIZE_T size;
    void *addr;

    TRACE("%p\n", heap );
    if (!heapPtr && heap && (((HEAP *)heap)->flags & HEAP_VALIDATE_PARAMS) &&
        NtCurrentTeb()->Peb->BeingDebugged)
    {
        DbgPrint( "Attempt to destroy an invalid heap\n" );
        DbgBreakPoint();
    }
    if (!heapPtr) return heap;

    if (heap == processHeap) return heap; /* cannot delete the main process heap */

    /* remove it from the per-process list */
    RtlEnterCriticalSection( &processHeap->cs );
    list_remove( &heapPtr->entry );
    RtlLeaveCriticalSection( &processHeap->cs );

    heapPtr->cs.DebugInfo->Spare[0] = 0;
    RtlDeleteCriticalSection( &heapPtr->cs );

    LIST_FOR_EACH_ENTRY_SAFE( arena, arena_next, &heapPtr->large_list, ARENA_LARGE, entry )
    {
        list_remove( &arena->entry );
        size = 0;
        addr = arena;
        NtFreeVirtualMemory( NtCurrentProcess(), &addr, &size, MEM_RELEASE );
    }
    LIST_FOR_EACH_ENTRY_SAFE( subheap, next, &heapPtr->subheap_list, SUBHEAP, entry )
    {
        if (subheap == &heapPtr->subheap) continue;  /* do this one last */
        notify_free_all( subheap );
        list_remove( &subheap->entry );
        size = 0;
        addr = ROUND_ADDR( subheap, COMMIT_MASK );
        NtFreeVirtualMemory( NtCurrentProcess(), &addr, &size, MEM_RELEASE );
    }
    notify_free_all( &heapPtr->subheap );
    RtlFreeHeap( GetProcessHeap(), 0, heapPtr->pending_free );
    size = 0;
    addr = heap;
    NtFreeVirtualMemory( NtCurrentProcess(), &addr, &size, MEM_RELEASE );
    return 0;
}

static NTSTATUS heap_allocate( HEAP *heap, ULONG flags, SIZE_T size, void **ret )
{
    SIZE_T old_block_size, block_size;
    struct block *block;
    SUBHEAP *subheap;

    block_size = sizeof(*block) + ROUND_SIZE(size) + HEAP_TAIL_EXTRA_SIZE(flags);
    if (block_size < size) return STATUS_NO_MEMORY;  /* overflow */
    if (block_size < HEAP_MIN_BLOCK_SIZE) block_size = HEAP_MIN_BLOCK_SIZE;

    if (block_size >= HEAP_MIN_LARGE_BLOCK_SIZE)
    {
        if (!(*ret = allocate_large_block( heap, flags, size ))) return STATUS_NO_MEMORY;
        return STATUS_SUCCESS;
    }

    /* Locate a suitable free block */

    if (!(block = find_free_block( heap, block_size, &subheap ))) return STATUS_NO_MEMORY;
    /* read the free block size, changing block type or flags may alter it */
    old_block_size = block_get_size( block );

    block_set_type( block, ARENA_INUSE_MAGIC );
    shrink_used_block( subheap, block, 0, old_block_size, block_size, size );

    notify_alloc( block + 1, size, flags & HEAP_ZERO_MEMORY );
    initialize_block( block + 1, size, block->unused_bytes, flags );

    *ret = block + 1;
    return STATUS_SUCCESS;
}

/***********************************************************************
 *           RtlAllocateHeap   (NTDLL.@)
 */
void *WINAPI DECLSPEC_HOTPATCH RtlAllocateHeap( HANDLE heap, ULONG flags, SIZE_T size )
{
    void *ptr = NULL;
    NTSTATUS status;
    HEAP *heapPtr;

    if (!(heapPtr = HEAP_GetPtr( heap )))
        status = STATUS_INVALID_HANDLE;
    else
    {
        heap_lock( heapPtr, flags );
        status = heap_allocate( heapPtr, heap_get_flags( heapPtr, flags ), size, &ptr );
        heap_unlock( heapPtr, flags );
    }

    TRACE( "heap %p, flags %#x, size %#Ix, return %p, status %#x.\n", heap, flags, size, ptr, status );
    heap_set_status( heapPtr, flags, status );
    return ptr;
}


static NTSTATUS heap_free( HEAP *heap, void *ptr )
{
    ARENA_INUSE *block;
    SUBHEAP *subheap;

    /* Inform valgrind we are trying to free memory, so it can throw up an error message */
    notify_free( ptr );

    if (!(block = unsafe_block_from_ptr( heap, ptr, &subheap ))) return STATUS_INVALID_PARAMETER;
    if (!subheap) free_large_block( heap, ptr );
    else free_used_block( subheap, block );

    return STATUS_SUCCESS;
}

/***********************************************************************
 *           RtlFreeHeap   (NTDLL.@)
 */
BOOLEAN WINAPI DECLSPEC_HOTPATCH RtlFreeHeap( HANDLE heap, ULONG flags, void *ptr )
{
    NTSTATUS status;
    HEAP *heapPtr;

    if (!ptr) return TRUE;

    if (!(heapPtr = HEAP_GetPtr( heap )))
        status = STATUS_INVALID_PARAMETER;
    else
    {
        heap_lock( heapPtr, flags );
        status = heap_free( heapPtr, ptr );
        heap_unlock( heapPtr, flags );
    }

    TRACE( "heap %p, flags %#x, ptr %p, return %u, status %#x.\n", heap, flags, ptr, !status, status );
    heap_set_status( heapPtr, flags, status );
    return !status;
}


static NTSTATUS heap_reallocate( HEAP *heap, ULONG flags, void *ptr, SIZE_T size, void **ret )
{
    SIZE_T old_block_size, old_size, block_size;
    struct block *next, *block;
    SUBHEAP *subheap;
    NTSTATUS status;

    block_size = sizeof(*block) + ROUND_SIZE(size) + HEAP_TAIL_EXTRA_SIZE(flags);
    if (block_size < size) return STATUS_NO_MEMORY;  /* overflow */
    if (block_size < HEAP_MIN_BLOCK_SIZE) block_size = HEAP_MIN_BLOCK_SIZE;

    if (!(block = unsafe_block_from_ptr( heap, ptr, &subheap ))) return STATUS_INVALID_PARAMETER;
    if (!subheap)
    {
        if (!(*ret = realloc_large_block( heap, flags, ptr, size ))) return STATUS_NO_MEMORY;
        return STATUS_SUCCESS;
    }

    /* Check if we need to grow the block */

    old_block_size = block_get_size( block );
    old_size = old_block_size - block_get_overhead( block );
    if (block_size > old_block_size)
    {
        if ((next = next_block( subheap, block )) && (block_get_flags( next ) & BLOCK_FLAG_FREE) &&
            block_size < HEAP_MIN_LARGE_BLOCK_SIZE && block_size <= old_block_size + block_get_size( next ))
        {
            /* The next block is free and large enough */
            struct entry *entry = (struct entry *)next;
            list_remove( &entry->entry );
            old_block_size += block_get_size( next );
            if (!subheap_commit( subheap, block, block_size )) return STATUS_NO_MEMORY;
            notify_realloc( block + 1, old_size, size );
            shrink_used_block( subheap, block, block_get_flags( block ), old_block_size, block_size, size );
        }
        else
        {
            if (flags & HEAP_REALLOC_IN_PLACE_ONLY) return STATUS_NO_MEMORY;
            if ((status = heap_allocate( heap, flags & ~HEAP_ZERO_MEMORY, size, ret ))) return status;
            memcpy( *ret, block + 1, old_size );
            if (flags & HEAP_ZERO_MEMORY) memset( (char *)*ret + old_size, 0, size - old_size );
            notify_free( ptr );
            free_used_block( subheap, block );
            return STATUS_SUCCESS;
        }
    }
    else
    {
        notify_realloc( block + 1, old_size, size );
        shrink_used_block( subheap, block, block_get_flags( block ), old_block_size, block_size, size );
    }

    /* Clear the extra bytes if needed */

    if (size <= old_size) mark_block_tail( (char *)(block + 1) + size, block->unused_bytes, flags );
    else initialize_block( (char *)(block + 1) + old_size, size - old_size, block->unused_bytes, flags );

    /* Return the new arena */

    *ret = block + 1;
    return STATUS_SUCCESS;
}

/***********************************************************************
 *           RtlReAllocateHeap   (NTDLL.@)
 */
void *WINAPI RtlReAllocateHeap( HANDLE heap, ULONG flags, void *ptr, SIZE_T size )
{
    void *ret = NULL;
    NTSTATUS status;
    HEAP *heapPtr;

    if (!ptr) return NULL;

    if (!(heapPtr = HEAP_GetPtr( heap )))
        status = STATUS_INVALID_HANDLE;
    else
    {
        heap_lock( heapPtr, flags );
        status = heap_reallocate( heapPtr, heap_get_flags( heapPtr, flags ), ptr, size, &ret );
        heap_unlock( heapPtr, flags );
    }

    TRACE( "heap %p, flags %#x, ptr %p, size %#Ix, return %p, status %#x.\n", heap, flags, ptr, size, ret, status );
    heap_set_status( heap, flags, status );
    return ret;
}


/***********************************************************************
 *           RtlCompactHeap   (NTDLL.@)
 *
 * Compact the free space in a Heap.
 *
 * PARAMS
 *  heap  [I] Heap that block was allocated from
 *  flags [I] HEAP_ flags from "winnt.h"
 *
 * RETURNS
 *  The number of bytes compacted.
 *
 * NOTES
 *  This function is a harmless stub.
 */
ULONG WINAPI RtlCompactHeap( HANDLE heap, ULONG flags )
{
    static BOOL reported;
    if (!reported++) FIXME( "(%p, 0x%x) stub\n", heap, flags );
    return 0;
}


/***********************************************************************
 *           RtlLockHeap   (NTDLL.@)
 *
 * Lock a Heap.
 *
 * PARAMS
 *  heap  [I] Heap to lock
 *
 * RETURNS
 *  Success: TRUE. The Heap is locked.
 *  Failure: FALSE, if heap is invalid.
 */
BOOLEAN WINAPI RtlLockHeap( HANDLE heap )
{
    HEAP *heapPtr = HEAP_GetPtr( heap );
    if (!heapPtr) return FALSE;
    heap_lock( heapPtr, 0 );
    return TRUE;
}


/***********************************************************************
 *           RtlUnlockHeap   (NTDLL.@)
 *
 * Unlock a Heap.
 *
 * PARAMS
 *  heap  [I] Heap to unlock
 *
 * RETURNS
 *  Success: TRUE. The Heap is unlocked.
 *  Failure: FALSE, if heap is invalid.
 */
BOOLEAN WINAPI RtlUnlockHeap( HANDLE heap )
{
    HEAP *heapPtr = HEAP_GetPtr( heap );
    if (!heapPtr) return FALSE;
    heap_unlock( heapPtr, 0 );
    return TRUE;
}


static NTSTATUS heap_size( HEAP *heap, const void *ptr, SIZE_T *size )
{
    const ARENA_INUSE *block;
    SUBHEAP *subheap;

    if (!(block = unsafe_block_from_ptr( heap, ptr, &subheap ))) return STATUS_INVALID_PARAMETER;
    if (!subheap)
    {
        const ARENA_LARGE *large_arena = (const ARENA_LARGE *)ptr - 1;
        *size = large_arena->data_size;
    }
    else *size = block_get_size( block ) - block_get_overhead( block );

    return STATUS_SUCCESS;
}

/***********************************************************************
 *           RtlSizeHeap   (NTDLL.@)
 */
SIZE_T WINAPI RtlSizeHeap( HANDLE heap, ULONG flags, const void *ptr )
{
    SIZE_T size = ~(SIZE_T)0;
    NTSTATUS status;
    HEAP *heapPtr;

    if (!(heapPtr = HEAP_GetPtr( heap )))
        status = STATUS_INVALID_PARAMETER;
    else
    {
        heap_lock( heapPtr, flags );
        status = heap_size( heapPtr, ptr, &size );
        heap_unlock( heapPtr, flags );
    }

    TRACE( "heap %p, flags %#x, ptr %p, return %#Ix, status %#x.\n", heap, flags, ptr, size, status );
    heap_set_status( heapPtr, flags, status );
    return size;
}


/***********************************************************************
 *           RtlValidateHeap   (NTDLL.@)
 */
BOOLEAN WINAPI RtlValidateHeap( HANDLE heap, ULONG flags, const void *ptr )
{
    SUBHEAP *subheap;
    HEAP *heapPtr;
    BOOLEAN ret;

    if (!(heapPtr = HEAP_GetPtr( heap )))
        ret = FALSE;
    else
    {
        heap_lock( heapPtr, flags );
        if (ptr) ret = heap_validate_ptr( heapPtr, ptr, &subheap );
        else ret = heap_validate( heapPtr );
        heap_unlock( heapPtr, flags );
    }

    TRACE( "heap %p, flags %#x, ptr %p, return %u.\n", heap, flags, ptr, !!ret );
    return ret;
}


static NTSTATUS heap_walk_blocks( const HEAP *heap, const SUBHEAP *subheap, struct rtl_heap_entry *entry )
{
    const char *base = subheap_base( subheap ), *commit_end = subheap_commit_end( subheap ), *end = base + subheap_size( subheap );
    const struct block *block, *blocks = first_block( subheap );

    if (entry->lpData == commit_end) return STATUS_NO_MORE_ENTRIES;

    if (entry->lpData == base) block = blocks;
    else if (!(block = next_block( subheap, (struct block *)entry->lpData - 1 )))
    {
        entry->lpData = (void *)commit_end;
        entry->cbData = end - commit_end;
        entry->cbOverhead = 0;
        entry->iRegionIndex = 0;
        entry->wFlags = RTL_HEAP_ENTRY_UNCOMMITTED;
        return STATUS_SUCCESS;
    }

    if (block_get_flags( block ) & BLOCK_FLAG_FREE)
    {
        entry->lpData = (char *)block + block_get_overhead( block );
        entry->cbData = block_get_size( block ) - block_get_overhead( block );
        /* FIXME: last free block should not include uncommitted range, which also has its own overhead */
        if (!contains( blocks, commit_end - (char *)blocks, block, block_get_size( block ) ))
            entry->cbData = commit_end - (char *)entry->lpData - 8 * sizeof(void *);
        entry->cbOverhead = 4 * sizeof(void *);
        entry->iRegionIndex = 0;
        entry->wFlags = 0;
    }
    else
    {
        entry->lpData = (void *)(block + 1);
        entry->cbData = block_get_size( block ) - block_get_overhead( block );
        entry->cbOverhead = block_get_overhead( block );
        entry->iRegionIndex = 0;
        entry->wFlags = RTL_HEAP_ENTRY_COMMITTED|RTL_HEAP_ENTRY_BLOCK|RTL_HEAP_ENTRY_BUSY;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS heap_walk( const HEAP *heap, struct rtl_heap_entry *entry )
{
    const ARENA_LARGE *large;
    const struct list *next;
    const SUBHEAP *subheap;
    NTSTATUS status;
    char *base;

    if ((large = find_large_block( heap, entry->lpData )))
        next = &large->entry;
    else if ((subheap = find_subheap( heap, entry->lpData, TRUE )))
    {
        if (!(status = heap_walk_blocks( heap, subheap, entry ))) return STATUS_SUCCESS;
        else if (status != STATUS_NO_MORE_ENTRIES) return status;
        next = &subheap->entry;
    }
    else
    {
        if (entry->lpData) return STATUS_INVALID_PARAMETER;
        next = &heap->subheap_list;
    }

    if (!large && (next = list_next( &heap->subheap_list, next )))
    {
        subheap = LIST_ENTRY( next, SUBHEAP, entry );
        base = subheap_base( subheap );
        entry->lpData = base;
        entry->cbData = subheap_overhead( subheap );
        entry->cbOverhead = 0;
        entry->iRegionIndex = 0;
        entry->wFlags = RTL_HEAP_ENTRY_REGION;
        entry->Region.dwCommittedSize = (char *)subheap_commit_end( subheap ) - base;
        entry->Region.dwUnCommittedSize = subheap_size( subheap ) - entry->Region.dwCommittedSize;
        entry->Region.lpFirstBlock = base + entry->cbData;
        entry->Region.lpLastBlock = base + subheap_size( subheap );
        return STATUS_SUCCESS;
    }

    if (!next) next = &heap->large_list;
    if ((next = list_next( &heap->large_list, next )))
    {
        large = LIST_ENTRY( next, ARENA_LARGE, entry );
        entry->lpData = (void *)(large + 1);
        entry->cbData = large->data_size;
        entry->cbOverhead = 0;
        entry->iRegionIndex = 64;
        entry->wFlags = RTL_HEAP_ENTRY_COMMITTED|RTL_HEAP_ENTRY_BLOCK|RTL_HEAP_ENTRY_BUSY;
        return STATUS_SUCCESS;
    }

    return STATUS_NO_MORE_ENTRIES;
}

/***********************************************************************
 *           RtlWalkHeap    (NTDLL.@)
 */
NTSTATUS WINAPI RtlWalkHeap( HANDLE heap, void *entry_ptr )
{
    struct rtl_heap_entry *entry = entry_ptr;
    NTSTATUS status;
    HEAP *heapPtr;

    if (!entry) return STATUS_INVALID_PARAMETER;

    if (!(heapPtr = HEAP_GetPtr(heap)))
        status = STATUS_INVALID_HANDLE;
    else
    {
        heap_lock( heapPtr, 0 );
        status = heap_walk( heapPtr, entry );
        heap_unlock( heapPtr, 0 );
    }

    TRACE( "heap %p, entry %p %s, return %#x\n", heap, entry,
           status ? "<empty>" : debugstr_heap_entry(entry), status );
    return status;
}


/***********************************************************************
 *           RtlGetProcessHeaps    (NTDLL.@)
 *
 * Get the Heaps belonging to the current process.
 *
 * PARAMS
 *  count [I] size of heaps
 *  heaps [O] Destination array for heap HANDLE's
 *
 * RETURNS
 *  Success: The number of Heaps allocated by the process.
 *  Failure: 0.
 */
ULONG WINAPI RtlGetProcessHeaps( ULONG count, HANDLE *heaps )
{
    ULONG total = 1;  /* main heap */
    struct list *ptr;

    RtlEnterCriticalSection( &processHeap->cs );
    LIST_FOR_EACH( ptr, &processHeap->entry ) total++;
    if (total <= count)
    {
        *heaps++ = processHeap;
        LIST_FOR_EACH( ptr, &processHeap->entry )
            *heaps++ = LIST_ENTRY( ptr, HEAP, entry );
    }
    RtlLeaveCriticalSection( &processHeap->cs );
    return total;
}

/***********************************************************************
 *           RtlQueryHeapInformation    (NTDLL.@)
 */
NTSTATUS WINAPI RtlQueryHeapInformation( HANDLE heap, HEAP_INFORMATION_CLASS info_class,
                                         PVOID info, SIZE_T size_in, PSIZE_T size_out)
{
    switch (info_class)
    {
    case HeapCompatibilityInformation:
        if (size_out) *size_out = sizeof(ULONG);

        if (size_in < sizeof(ULONG))
            return STATUS_BUFFER_TOO_SMALL;

        *(ULONG *)info = 0; /* standard heap */
        return STATUS_SUCCESS;

    default:
        FIXME("Unknown heap information class %u\n", info_class);
        return STATUS_INVALID_INFO_CLASS;
    }
}

/***********************************************************************
 *           RtlSetHeapInformation    (NTDLL.@)
 */
NTSTATUS WINAPI RtlSetHeapInformation( HANDLE heap, HEAP_INFORMATION_CLASS info_class, PVOID info, SIZE_T size)
{
    FIXME("%p %d %p %ld stub\n", heap, info_class, info, size);
    return STATUS_SUCCESS;
}

/***********************************************************************
 *           RtlGetUserInfoHeap    (NTDLL.@)
 */
BOOLEAN WINAPI RtlGetUserInfoHeap( HANDLE heap, ULONG flags, void *ptr, void **user_value, ULONG *user_flags )
{
    FIXME( "heap %p, flags %#x, ptr %p, user_value %p, user_flags %p semi-stub!\n",
           heap, flags, ptr, user_value, user_flags );
    *user_value = NULL;
    *user_flags = 0;
    return TRUE;
}

/***********************************************************************
 *           RtlSetUserValueHeap    (NTDLL.@)
 */
BOOLEAN WINAPI RtlSetUserValueHeap( HANDLE heap, ULONG flags, void *ptr, void *user_value )
{
    FIXME( "heap %p, flags %#x, ptr %p, user_value %p stub!\n", heap, flags, ptr, user_value );
    return FALSE;
}

/***********************************************************************
 *           RtlSetUserFlagsHeap    (NTDLL.@)
 */
BOOLEAN WINAPI RtlSetUserFlagsHeap( HANDLE heap, ULONG flags, void *ptr, ULONG clear, ULONG set )
{
    FIXME( "heap %p, flags %#x, ptr %p, clear %#x, set %#x stub!\n", heap, flags, ptr, clear, set );
    return FALSE;
}
