/*-------------------------------------------------------------------------
 *
 * nodescan.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef VECTOR_ENGINE_NODE_SCAN_H
#define VECTOR_ENGINE_NODE_SCAN_H

#include "nodes/execnodes.h"
#include "nodes/plannodes.h"

/*
 * VectorScanState - state object of vectorscan on executor.
 */
typedef struct VectorScanState
{
	CustomScanState	css;

	/* Attributes for vectorization */
	SeqScanState	*seqstate;
	bool		     scanFinish;
	Bitmapset       *usedColumns;
	TupleTableSlot  *slot;
} VectorScanState;

extern CustomScan *MakeCustomScanForSeqScan(void);
extern void InitVectorScan(void);

#endif   /* VECTOR_ENGINE_SCAN_H */
