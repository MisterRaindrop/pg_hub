/*-------------------------------------------------------------------------
 *
 * lwlock/bench.c
 *		Micro-benchmark of LWLock acquire/release paths.
 *
 * IDENTIFICATION
 *	  src/test/modules/microbench/lwlock/bench.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "funcapi.h"
#include "portability/instr_time.h"
#include "storage/buf_internals.h"
#include "storage/lwlock.h"
#include "utils/builtins.h"
#include "utils/tuplestore.h"

#include "randomize.h"
#include "timing-magic.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(bench_lwlock);

/*
 * bench_lwlock(n, rounds, randomize) -> SETOF
 *     (op text, avg_ns float8, batch_size int8, id int8)
 */
Datum
bench_lwlock(PG_FUNCTION_ARGS)
{
	int64			n = PG_GETARG_INT64(0);
	int64			rounds = PG_ARGISNULL(1) ? 1 : PG_GETARG_INT64(1);
	bool			randomize = PG_ARGISNULL(2) ? true : PG_GETARG_BOOL(2);
	ReturnSetInfo  *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	LWLock		  **locks;
	volatile int64	sink PG_USED_FOR_ASSERTS_ONLY = 0;

	if (n <= 0 || rounds <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("n and rounds must be positive")));

	if (n > NUM_BUFFER_PARTITIONS)
		n = NUM_BUFFER_PARTITIONS;

	InitMaterializedSRF(fcinfo, 0);

	locks = palloc(sizeof(LWLock *) * n);
	if (randomize)
	{
		pg_prng_state rng;

		pg_prng_seed(&rng, 0xDA7ABA5E);
		for (int i = 0; i < n; i++)
			locks[i] = BufMappingPartitionLock(i);
		shuffle_pointers(&rng, (void **) locks, (int) n);
	}
	else
	{
		for (int i = 0; i < n; i++)
			locks[i] = BufMappingPartitionLock(0);
	}

	if (!timing_initialized)
		pg_initialize_timing();

	for (int64 r = 0; r < rounds; r++)
	{
		INIT_TIMING_SCOPE();
		{
			BufferDesc *buf_desc = GetBufferDescriptor(1);
			BEGIN_TIMING("spin-lock", n)
			{
				LockBufHdr(buf_desc);
				UnlockBufHdr(buf_desc);
			}
			END_TIMING;
		}

		BEGIN_TIMING("LWLock-ex", n)
		{
			LWLock	   *lock = locks[i];

			LWLockAcquire(lock, LW_EXCLUSIVE);
			LWLockRelease(lock);
		}
		END_TIMING;

		BEGIN_TIMING("LWLock-sh", n)
		{
			LWLock	   *lock = locks[i];

			LWLockAcquire(lock, LW_SHARED);
			LWLockRelease(lock);
		}
		END_TIMING;

		BEGIN_TIMING("LWLock-cond", n)
		{
			LWLock	   *lock = locks[i];

			if (!LWLockConditionalAcquire(lock, LW_SHARED))
				elog(ERROR, "Failed to acquire lock");
			LWLockRelease(lock);
		}
		END_TIMING;

		BEGIN_TIMING("nop", n)
		{
			LWLock	   *lock = locks[i];

			sink += (int64) (uintptr_t) lock;
		}
		END_TIMING;
	}

	(void) sink;
	return (Datum) 0;
}
