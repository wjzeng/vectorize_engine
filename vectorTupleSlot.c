#include "postgres.h"

#include "access/sysattr.h"
#include "access/tuptoaster.h"
#include "executor/tuptable.h"
#include "utils/expandeddatum.h"

#include "vectorTupleSlot.h"


static void Vslot_deform_tuple(TupleTableSlot *slot, int natts);
extern void _vslot_getsomeattrs(TupleTableSlot *slot, int attnum);
/* --------------------------------
 *		VMakeTupleTableSlot
 *
 *		Basic routine to make an empty VectorTupleTableSlot.
 * --------------------------------
 */
TupleTableSlot *
VMakeTupleTableSlot(void)
{
	TupleTableSlot		*slot;
	VectorTupleSlot		*vslot;

	slot = palloc(sizeof(VectorTupleSlot));
	NodeSetTag(slot, T_TupleTableSlot);

	init_slot(slot, NULL);

	/* vectorized fields */
	vslot = (VectorTupleSlot*)slot;
	vslot->dim = 0;
	vslot->bufnum = 0;
	memset(vslot->tts_buffers, InvalidBuffer, sizeof(vslot->tts_buffers));
	memset(vslot->tts_tuples, 0, sizeof(vslot->tts_tuples));
	/* all tuples should be skipped in initialization */
	memset(vslot->skip, true, sizeof(vslot->skip));

	return slot;
}

/* --------------------------------
 *		VExecAllocTableSlot
 *
 *		Create a vector tuple table slot within a tuple table (which is just a List).
 * --------------------------------
 */
TupleTableSlot *
VExecAllocTableSlot(List **tupleTable)
{
	TupleTableSlot *slot = VMakeTupleTableSlot();

	*tupleTable = lappend(*tupleTable, slot);

	return slot;
}


/*
 * slot_deform_tuple
 *		Given a TupleTableSlot, extract data from the slot's physical tuple
 *		into its Datum/isnull arrays.  Data is extracted up through the
 *		natts'th column (caller must ensure this is a legal column number).
 *
 *		This is essentially an incremental version of heap_deform_tuple:
 *		on each call we extract attributes up to the one needed, without
 *		re-computing information about previously extracted attributes.
 *		slot->tts_nvalid is the number of attributes already extracted.
 */
static void
Vslot_deform_tuple(TupleTableSlot *slot, int natts)
{
	VectorTupleSlot	*vslot = (VectorTupleSlot *)slot;
	HeapTuple	tuple;// = TupGetHeapTuple(slot); 
	TupleDesc	tupleDesc = slot->tts_tupleDescriptor;
	HeapTupleHeader tup;// = tuple->t_data;
	bool		hasnulls;// = HeapTupleHasNulls(tuple);
	Form_pg_attribute *att = tupleDesc->attrs;
	int			attnum;
	char	   *tp;				/* ptr to tuple data */
	long		off;			/* offset in tuple data */
	bits8	   *bp;// = tup->t_bits;		/* ptr to null bitmap in tuple */
	bool		slow;			/* can we use/set attcacheoff? */
	int			row;
	vtype		*column;

	for (row = 0; row < vslot->dim; row++)
	{
		tuple = &vslot->tts_tuples[row];
		tup = tuple->t_data;
		bp = tup->t_bits;
		hasnulls = HeapTupleHasNulls(tuple);

		/*
		 * Check whether the first call for this tuple, and initialize or restore
		 * loop state.
		 */
		attnum = slot->PRIVATE_tts_nvalid;
		/* vectorize engine deform once for now */
		off = 0;
		slow = false;

		tp = (char *) tup + tup->t_hoff;

		for (; attnum < natts; attnum++)
		{
			Form_pg_attribute thisatt = att[attnum];
			column = (vtype *)slot->PRIVATE_tts_values[attnum];

			if (hasnulls && att_isnull(attnum, bp))
			{
				column->values[row] = (Datum) 0;
				column->isnull[row] = true;
				slow = true;		/* can't use attcacheoff anymore */
				continue;
			}

			column->isnull[row] = false;

			if (!slow && thisatt->attcacheoff >= 0)
				off = thisatt->attcacheoff;
			else if (thisatt->attlen == -1)
			{
				/*
				 * We can only cache the offset for a varlena attribute if the
				 * offset is already suitably aligned, so that there would be no
				 * pad bytes in any case: then the offset will be valid for either
				 * an aligned or unaligned value.
				 */
				if (!slow &&
						off == att_align_nominal(off, thisatt->attalign))
					thisatt->attcacheoff = off;
				else
				{
					off = att_align_pointer(off, thisatt->attalign, -1,
							tp + off);
					slow = true;
				}
			}
			else
			{
				/* not varlena, so safe to use att_align_nominal */
				off = att_align_nominal(off, thisatt->attalign);

				if (!slow)
					thisatt->attcacheoff = off;
			}
			if (!slow && thisatt->attlen < 0)
				slow = true;

			column->values[row] = fetchatt(thisatt, tp + off);

			off = att_addlength_pointer(off, thisatt->attlen, tp + off);

			if (thisatt->attlen <= 0)
				slow = true;		/* can't use attcacheoff anymore */
		}
	}

	attnum = slot->PRIVATE_tts_nvalid;
	for (; attnum < natts; attnum++)
	{
		column = (vtype *)slot->PRIVATE_tts_values[attnum];
		column->dim = vslot->dim;
	}

	/*
	 * Save state for next execution
	 */
	slot->PRIVATE_tts_nvalid = attnum;
}

/*
 * slot_getsomeattrs
 *		This function forces the entries of the slot's Datum/isnull
 *		arrays to be valid at least up through the attnum'th entry.
 */
void
_vslot_getsomeattrs(TupleTableSlot *slot, int attnum)
{
	VectorTupleSlot	*vslot = (VectorTupleSlot *)slot;
	HeapTuple	tuple;
	int			attno;
	int			i;

	/* Quick out if we have 'em all already */
	if (slot->PRIVATE_tts_nvalid >= attnum)
		return;

	if (vslot->dim == 0)
		return;
	/* Check for caller error */
	if (attnum <= 0 || attnum > slot->tts_tupleDescriptor->natts)
		elog(ERROR, "invalid attribute number %d", attnum);

	/*
	 * otherwise we had better have a physical tuple (tts_nvalid should equal
	 * natts in all virtual-tuple cases)
	 */
	for (i = 0; i < vslot->dim; i++)
	{
		tuple = &vslot->tts_tuples[i];
		if (tuple == NULL)			/* internal error */
			elog(ERROR, "cannot extract attribute from empty tuple slot");
	}

	/*
	 * load up any slots available from physical tuple
	 */
	attno = HeapTupleHeaderGetNatts(vslot->tts_tuples[0].t_data);
	attno = Min(attno, attnum);

	Vslot_deform_tuple(slot, attno);

	/*
	 * If tuple doesn't have all the atts indicated by tupleDesc, read the
	 * rest as null
	 */
	for (; attno < attnum; attno++)
	{
		slot->PRIVATE_tts_values[attno] = (Datum) 0;
		slot->PRIVATE_tts_isnull[attno] = true;
	}
	slot->PRIVATE_tts_nvalid = attnum;
	TupSetVirtualTuple(slot);
}

/*
 * slot_getallattrs
 *		This function forces all the entries of the slot's Datum/isnull
 *		arrays to be valid.  The caller may then extract data directly
 *		from those arrays instead of using slot_getattr.
 */
void
Vslot_getallattrs(TupleTableSlot *slot)
{
	Vslot_getsomeattrs(slot, slot->tts_tupleDescriptor->natts);
}


/*
 * slot_getsomeattrs
 *		This function forces the entries of the slot's Datum/isnull
 *		arrays to be valid at least up through the attnum'th entry.
 */
void
Vslot_getsomeattrs(TupleTableSlot *slot, int attnum)
{
	if(TupHasVirtualTuple(slot))
	{
		if(slot->PRIVATE_tts_nvalid >= attnum)
			return;
	}

	if(TupHasMemTuple(slot))
	{
		int i; 
		for(i=0; i<attnum; ++i)
			slot->PRIVATE_tts_values[i] = memtuple_getattr(
					slot->PRIVATE_tts_memtuple, slot->tts_mt_bind, 
					i+1, &(slot->PRIVATE_tts_isnull[i]));

		TupSetVirtualTuple(slot);
		slot->PRIVATE_tts_nvalid = attnum;
		return;
	}

	_vslot_getsomeattrs(slot, attnum); 
	TupSetVirtualTuple(slot);
}


/* --------------------------------
 *		VExecClearTuple
 *
 *		This function is used to clear out a slot in the tuple table.
 *
 *		NB: only the tuple is cleared, not the tuple descriptor (if any).
 * --------------------------------
 */
TupleTableSlot *
VExecClearTuple(TupleTableSlot *slot)	/* slot in which to store tuple */
{
	int				i;
	vtype			*column;
	VectorTupleSlot *vslot;
	/*
	 * sanity checks
	 */
	Assert(slot != NULL);

	/*
	 * Free the old physical tuple if necessary.
	 */
	free_heaptuple_memtuple(slot);

	slot->PRIVATE_tts_flags = TTS_ISEMPTY;
	slot->PRIVATE_tts_nvalid = 0;

	/*
	 * Drop the pin on the referenced buffer, if there is one.
	 */
	if (BufferIsValid(slot->tts_buffer))
		ReleaseBuffer(slot->tts_buffer);

	slot->tts_buffer = InvalidBuffer;
	
	/* vectorize part  */
	vslot = (VectorTupleSlot *)slot;
	for(i = 0; i < vslot->bufnum; i++)
	{
		if(BufferIsValid(vslot->tts_buffers[i]))
		{
			ReleaseBuffer(vslot->tts_buffers[i]);
			vslot->tts_buffers[i] = InvalidBuffer;
		}
	}
	vslot->dim = 0;
	vslot->bufnum = 0;

	for (i = 0; i < slot->tts_tupleDescriptor->natts; i++)
	{
		column = (vtype *)DatumGetPointer(slot->PRIVATE_tts_values[i]);
		column->dim = 0;
	}

	memset(vslot->skip, true, sizeof(vslot->skip));

	return slot;
}


/* --------------------------------
 *		ExecStoreTuple
 *
 *		This function is used to store a physical tuple into a specified
 *		slot in the tuple table.
 *
 *		tuple:	tuple to store
 *		slot:	slot to store it in
 *		buffer: disk buffer if tuple is in a disk page, else InvalidBuffer
 *		shouldFree: true if ExecClearTuple should pfree() the tuple
 *					when done with it
 *
 * If 'buffer' is not InvalidBuffer, the tuple table code acquires a pin
 * on the buffer which is held until the slot is cleared, so that the tuple
 * won't go away on us.
 *
 * shouldFree is normally set 'true' for tuples constructed on-the-fly.
 * It must always be 'false' for tuples that are stored in disk pages,
 * since we don't want to try to pfree those.
 *
 * Another case where it is 'false' is when the referenced tuple is held
 * in a tuple table slot belonging to a lower-level executor Proc node.
 * In this case the lower-level slot retains ownership and responsibility
 * for eventually releasing the tuple.  When this method is used, we must
 * be certain that the upper-level Proc node will lose interest in the tuple
 * sooner than the lower-level one does!  If you're not certain, copy the
 * lower-level tuple with heap_copytuple and let the upper-level table
 * slot assume ownership of the copy!
 *
 * Return value is just the passed-in slot pointer.
 *
 * NOTE: before PostgreSQL 8.1, this function would accept a NULL tuple
 * pointer and effectively behave like ExecClearTuple (though you could
 * still specify a buffer to pin, which would be an odd combination).
 * This saved a couple lines of code in a few places, but seemed more likely
 * to mask logic errors than to be really useful, so it's now disallowed.
 * --------------------------------
 */
TupleTableSlot *
VExecStoreTuple(HeapTuple tuple,
			   TupleTableSlot *slot,
			   Buffer buffer,
			   bool shouldFree)
{
	VectorTupleSlot *vslot;
	/*
	 * sanity checks
	 */
	Assert(tuple != NULL);
	Assert(slot != NULL);
	Assert(slot->tts_tupleDescriptor != NULL);
	/* passing shouldFree=true for a tuple on a disk page is not sane */
	Assert(BufferIsValid(buffer) ? (!shouldFree) : true);

	Assert(!is_memtuple((GenericTuple) tuple));

	vslot = (VectorTupleSlot *)slot;
	/*
	 * Free any old physical tuple belonging to the slot.
	 */
	free_heaptuple_memtuple(slot);

	/*
	 * Store the new tuple into the specified slot.
	 */

	/* Clear tts_flags, here isempty set to false */
	slot->PRIVATE_tts_flags = shouldFree ? TTS_SHOULDFREE : 0;

	/* store the tuple */
	memcpy(&vslot->tts_tuples[vslot->dim], tuple, sizeof(HeapTupleData));

	/* Mark extracted state invalid */
	slot->PRIVATE_tts_nvalid = 0;

	/*
	 * If tuple is on a disk page, keep the page pinned as long as we hold a
	 * pointer into it.  We assume the caller already has such a pin.
	 *
	 * This is coded to optimize the case where the slot previously held a
	 * tuple on the same disk page: in that case releasing and re-acquiring
	 * the pin is a waste of cycles.  This is a common situation during
	 * seqscans, so it's worth troubling over.
	 */
	if (vslot->bufnum == 0 || vslot->tts_buffers[vslot->bufnum-1] != buffer)
	{
		if (BufferIsValid(vslot->tts_buffers[vslot->bufnum]))
			ReleaseBuffer(vslot->tts_buffers[vslot->bufnum]);
		vslot->tts_buffers[vslot->bufnum] = buffer;
		vslot->bufnum++;
		if (BufferIsValid(buffer))
			IncrBufferRefCount(buffer);
	}
	vslot->dim++;

	return slot;
}

void
InitializeVectorSlotColumn(VectorTupleSlot *vslot)
{
	TupleDesc	desc;
	Oid			typid;
	vtype		*column;
	int			i;

	desc = vslot->tts.tts_tupleDescriptor;
	/* initailize column in vector slot */
	for (i = 0; i < desc->natts; i++)
	{
		typid = desc->attrs[i]->atttypid;
		column = buildvtype(typid, BATCHSIZE, vslot->skip);
		column->dim = 0;
		vslot->tts.PRIVATE_tts_values[i]  = PointerGetDatum(column);
		/* PRIVATE_tts_isnull not used yet */
		vslot->tts.PRIVATE_tts_isnull[i] = false;
	}
}
