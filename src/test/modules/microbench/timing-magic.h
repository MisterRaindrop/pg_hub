#ifndef MICROBENCH_TIMING_MAGIC_H
#define MICROBENCH_TIMING_MAGIC_H

#include "portability/instr_time.h"

#define INIT_TIMING_SCOPE() \
	int64 timing_operation_id = 0

#define BEGIN_TIMING(name, n) \
	do { \
		instr_time t0, t1, dt; \
		Datum values[4]; \
		bool nulls[4] = {0}; \
		values[0] = CStringGetTextDatum(name); \
		INSTR_TIME_SET_CURRENT_FAST(t0); \
		for (int64 i = 0; i < (n); ++i) \
		{

#define END_TIMING \
		} \
		INSTR_TIME_SET_CURRENT_FAST(t1); \
		INSTR_TIME_SET_ZERO(dt); \
		INSTR_TIME_ACCUM_DIFF(dt, t1, t0); \
		values[1] = Float8GetDatum((double) INSTR_TIME_GET_NANOSEC(dt) / (double) (n)); \
		values[2] = Int64GetDatum(n); \
		values[3] = Int64GetDatum(++timing_operation_id); \
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls); \
	} while (0)

#endif
