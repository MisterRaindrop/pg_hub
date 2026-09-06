/*-------------------------------------------------------------------------
 *
 * buf_table.c
 *	  routines for mapping BufferTags to buffer indexes.
 *
 * The shared buffer mapping table is a flat, index-linked hash table (an
 * open-chaining replacement for the former dynahash-based table).  It is made
 * of two shared-memory arrays:
 *
 *	  buckets[num_buckets] - one chain head per hash bucket
 *	  entries[NBuffers]    - one entry per buffer, indexed by buf_id
 *
 * Each buffer slot i permanently owns entry slot i, so no freelist is needed:
 * bufmgr always removes a buffer's old mapping (BufTableDelete, called from
 * InvalidateVictimBuffer) before inserting a new tag for that same buf_id (see
 * GetVictimBuffer / BufferAlloc in bufmgr.c).  Chains are linked by int index
 * and terminated by P_NEW.
 *
 * num_buckets is a power of two and a multiple of NUM_BUFFER_PARTITIONS, so the
 * bucket index (hashcode % num_buckets) shares its low bits with the partition
 * index (hashcode % NUM_BUFFER_PARTITIONS).  Every tag that maps to a given
 * bucket therefore maps to a single partition.
 *
 * BufTableInsert and BufTableDelete must be called with the tag's
 * BufMappingPartitionLock held exclusively, so a writer is the only mutator of
 * its bucket and both are plain list manipulations.
 *
 * BufTableLookup holds no lock.  Each entry caches its hashcode, and deletion
 * complements it before unlinking; complementing always changes the bucket
 * bits, so an unlinked or recycled entry no longer claims membership of this
 * bucket and a scanner that reaches one knows its link is untrustworthy and
 * starts over.
 *
 * Either outcome is acceptable when a lookup runs concurrently with an insert
 * or delete, and the result is a hint in any case: the buffer can be evicted as
 * soon as we return, so the caller must pin it and recheck its tag (as
 * ReadRecentBuffer does).  BufTableInsert is authoritative.
 *
 * Note: the entry arrays are never initialized.  An entry is only ever read
 * while linked into a chain, and it is fully written before being linked.
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/buffer/buf_table.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/hashfn.h"
#include "miscadmin.h"
#include "port/atomics.h"
#include "port/pg_bitutils.h"
#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include "storage/shmem.h"
#include "storage/subsystems.h"

/* entry for buffer lookup hashtable */
typedef struct
{
	BufferTag			tag;		/* Tag of a disk page */
	uint32				hashcode;	/* tag's hash code, complemented if unlinked */
	uint32				bucket;		/* which is this node inserted */
	pg_atomic_uint32	next;		/* next entry in hash chain, or P_NEW */
} pg_attribute_aligned(32) BufferLookupEnt;

/* bucket and entry arrays for buffer lookup hashtable (in shared memory) */
static pg_atomic_uint32 *buckets;
static BufferLookupEnt *entries;

StaticAssertDecl(
	sizeof(BufferLookupEnt) == 32,
	"BufferLookupEnt must be 32-bytes"
);

/* number of hash buckets; power of two and multiple of NUM_BUFFER_PARTITIONS */
static int	num_buckets;
static int	bucket_mask;
static void BufTableShmemRequest(void *arg);
static void BufTableShmemInit(void *arg);
static void BufTableShmemAttach(void *arg);

const ShmemCallbacks BufTableShmemCallbacks = {
	.request_fn = BufTableShmemRequest,
	.init_fn = BufTableShmemInit,
	.attach_fn = BufTableShmemAttach,
};

/*
 * Number of hash buckets for the current NBuffers.
 *
 * Must be a power of two (so hashcode % num_buckets == hashcode & (num_buckets
 * - 1)) and a multiple of NUM_BUFFER_PARTITIONS, so that every tag in a bucket
 * maps to a single buffer partition (see file header).
 */
static inline int
BufTableNumBuckets(void)
{
	return Max(NUM_BUFFER_PARTITIONS, pg_nextpower2_32(1.5 * NBuffers));
}

/*
 * Register shared memory arrays for mapping buffers.
 */
void
BufTableShmemRequest(void *arg)
{
	num_buckets = BufTableNumBuckets();
	bucket_mask = num_buckets - 1;
	Assert(num_buckets % NUM_BUFFER_PARTITIONS == 0);

	ShmemRequestStruct(.name = "Shared Buffer Lookup Buckets",
					   .size = (Size) num_buckets * sizeof(pg_atomic_uint32),
					   .ptr = (void **) &buckets,
		);

	ShmemRequestStruct(.name = "Shared Buffer Lookup Entries",
					   .size = (Size) NBuffers * sizeof(BufferLookupEnt),
					   .ptr = (void **) &entries,
		);
}

/*
 * Initialize the shared buffer lookup table.  Called once during shared-memory
 * initialization (in the postmaster, or in a standalone backend).
 *
 * Shared memory is zeroed, but zero is a valid buf_id, so we must explicitly
 * mark every bucket empty.
 */
void
BufTableShmemInit(void *arg)
{
	num_buckets = BufTableNumBuckets();
	bucket_mask = num_buckets - 1;
	for (int i = 0; i < num_buckets; i++)
		pg_atomic_init_u32(&buckets[i], P_NEW);
}

/*
 * Per-backend attach.  The buckets/entries pointers are restored by the shmem
 * framework, but num_buckets is a process-local scalar that must be recomputed
 * in each backend.  Forked children inherit it, but EXEC_BACKEND children run
 * only the attach callback, so set it here too.
 */
void
BufTableShmemAttach(void *arg)
{
	num_buckets = BufTableNumBuckets();
	bucket_mask = num_buckets - 1;
}

/*
 * BufTableHashCode
 *		Compute the hash code associated with a BufferTag
 *
 * This must be passed to the lookup/insert/delete routines along with the
 * tag.  We do it like this because the callers need to know the hash code
 * in order to determine which buffer partition to lock, and we don't want
 * to do the hash computation twice (hash_any is a bit slow).
 */
uint32
BufTableHashCode(BufferTag *tagPtr)
{
	return tag_hash(tagPtr, sizeof(BufferTag));
}

static inline uint32
pg_atomic_fetch_u32(volatile pg_atomic_uint32 *ptr)
{
#if defined(HAVE_GCC__ATOMIC_INT32_CAS)
    return __atomic_load_n(&ptr->value, __ATOMIC_ACQUIRE);
#else
    return pg_atomic_fetch_add_u32_impl(ptr, 0);  /* fallback RMW */
#endif
}

/* BufTableScan
 *		Helper for lookup and insertion
 *
 * Scan table following links optimistically. At some point
 * it might hold reference to a link from a node already deleted
 * that link is valid until the node is recycled. Once the node
 * is recycled it can be either (1) inserted on a different bucket
 * in which case we detect by comparing the bucket of the last
 * visited entry; (2) inserted on the same bucket, in which case
 * it will be before all the nodes that previously succeded it;
 *
 * what about constructing a chain (id, bucket) as 
 * (1,a) -> (2,b) -> (3,a).
 * (1,a) -> (2,b) requires 2 being after 1 on bucket a, then removed
 * and inserted on bucket b before (3,a).
 * but in order to have however if 3 was on bucket b then it should
 * be (3,b), so the only possibility is that 3 was on bucket a, at
 * the moment the bucket was inserted.
 * 
 */
static pg_always_inline uint32
BufTableScan(BufferTag *tagPtr, uint32 hashcode, int buf_id)
{

	uint32			  bucket;
	pg_atomic_uint32 *head;
	uint32			  id;
	uint32			  prev;
	uint32			  next;
	uint32			  attempts;

	bucket = hashcode & bucket_mask;
	head = &buckets[bucket];
	attempts = 0;
retry:
	if(++attempts > 1000) goto die;
	prev = P_NEW;
	id = pg_atomic_fetch_u32(head);
	while(id != P_NEW)
	{
		/* this will get a fresh version of entry cache line */
		next = pg_atomic_fetch_u32(&entries[id].next);
		if (BufferTagsEqual(&entries[id].tag, tagPtr))
			return id;
		if(entries[id].bucket != bucket)
		{
			if(prev != P_NEW && entries[prev].bucket == bucket)
			{
				/* step back for a while and try the same link again */
				id = prev;
				SPIN_DELAY(); 
				continue;
			}
			else
				goto retry;
		}else{
			prev = id;
			id = next;
		}
	}
	if(buf_id == P_NEW) return P_NEW;

	/* Prepare the entry */
	entries[buf_id].tag = *tagPtr;
	entries[buf_id].hashcode = hashcode;
	entries[buf_id].bucket = bucket;
	id = pg_atomic_read_u32(head);

	/* atttach the entry to the chain */
	do {
		pg_atomic_write_u32(&entries[buf_id].next, id);
		pg_write_barrier();
	} while(!pg_atomic_compare_exchange_u32(head, &id, buf_id));
	return P_NEW;
die:
	elog(ERROR, "corrupted chain, chain starting in bucket %d ended in bucket %d",
		bucket, entries[prev].bucket);
}
/*
 * BufTableLookup
 *		Lookup the given BufferTag; return buffer ID, or -1 if not found
 *
 * Takes no lock; see the file header for what the caller owes us.
 */
int
BufTableLookup(BufferTag *tagPtr, uint32 hashcode)
{
	return BufTableScan(tagPtr, hashcode, P_NEW);
}

/*
 * BufTableInsert
 *		Insert a hashtable entry for given tag and buffer ID,
 *		unless an entry already exists for that tag
 *
 * Shared data integrity is guaranteed, the operation is atomic
 * without intermediate states.
 * Returns -1 on successful insertion, or the id, if already
 * present.
 */
int
BufTableInsert(BufferTag *tagPtr, uint32 hashcode, int buf_id)
{
	return BufTableScan(tagPtr, hashcode, buf_id);
}

/*
 * BufTableDelete
 *		Delete the hashtable entry for given buffer (which must exist)
 *
 * This function operates atomically, however when deleting a node
 * N it assumes both prev(N) and next(N) to remain in the list until
 * the deletion of N is completed. To satisfy this condition we need
 * the caller must prevent concurrent calls to BufTableDelete on the
 * same bucket. e.g. holding an exclusive lock.
 */
void
BufTableDelete(BufferTag *tag, uint32 hashcode)
{
	uint32			  	 bucket = hashcode & bucket_mask;
	pg_atomic_uint32	*link;
	uint32				 id;
	Assert(LWLockHeldByMeInMode(BufMappingPartitionLock(hashcode),
								LW_EXCLUSIVE));
retry:
	for (link = &buckets[bucket];
		(id = pg_atomic_read_u32(link)) != P_NEW;
		link = &entries[id].next)
	{
		pg_read_barrier();
		if (entries[id].hashcode == hashcode &&
			BufferTagsEqual(&entries[id].tag, tag))
		{
			/* XXX: memory barriers?? we are already making a decision
			 * assuming currency of .tag, so just continue with that */
			uint32 next = pg_atomic_read_u32(&entries[id].next);
			if(pg_atomic_compare_exchange_u32(link, &id, next))
				return;
			/*
			 * the link changed, this entry can't be recycled yet
			 * so the only possibility is that it was linked from
			 * bucket head, and a concurrent insertion changed it.
			 * restart from scratch, it should not be very far from
			 * the start.
			 */
			goto retry;
		}
	}
	elog(ERROR, "tag not in shared buffer mapping table");
}
