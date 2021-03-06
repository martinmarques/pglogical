/*-------------------------------------------------------------------------
 *
 * pglogical_proto_native.c
 * 		pglogical binary protocol functions
 *
 * Copyright (c) 2015, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  pglogical_proto_native.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "pglogical_output.h"

#include "access/sysattr.h"
#include "access/tuptoaster.h"
#include "catalog/pg_type.h"
#include "libpq/pqformat.h"
#include "nodes/parsenodes.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "pglogical_output_internal.h"
#include "pglogical_relmetacache.h"
#include "pglogical_proto_native.h"

#define IS_REPLICA_IDENTITY 1

static void pglogical_write_attrs(StringInfo out, Relation rel,
								  Bitmapset *att_filter);
static void pglogical_write_tuple(StringInfo out, PGLogicalOutputData *data,
								  Relation rel, HeapTuple tuple,
								  Bitmapset *att_filter);
static char decide_datum_transfer(Form_pg_attribute att,
								  Form_pg_type typclass,
								  bool allow_internal_basetypes,
								  bool allow_binary_basetypes);

/*
 * Write relation description to the output stream.
 */
void
pglogical_write_rel(StringInfo out, PGLogicalOutputData *data, Relation rel,
		PGLRelMetaCacheEntry *cache_entry, Bitmapset *att_filter)
{
	char	   *nspname;
	uint8		nspnamelen;
	const char *relname;
	uint8		relnamelen;
	uint8		flags = 0;

	/* must never be called with an already-cached rel */
	Assert(cache_entry == NULL || !cache_entry->is_cached);

	pq_sendbyte(out, 'R');		/* sending RELATION */

	/* send the flags field */
	pq_sendbyte(out, flags);

	/* use Oid as relation identifier */
	pq_sendint(out, RelationGetRelid(rel), 4);

	nspname = get_namespace_name(rel->rd_rel->relnamespace);
	if (nspname == NULL)
		elog(ERROR, "cache lookup failed for namespace %u",
			 rel->rd_rel->relnamespace);
	nspnamelen = strlen(nspname) + 1;

	relname = NameStr(rel->rd_rel->relname);
	relnamelen = strlen(relname) + 1;

	pq_sendbyte(out, nspnamelen);		/* schema name length */
	pq_sendbytes(out, nspname, nspnamelen);

	pq_sendbyte(out, relnamelen);		/* table name length */
	pq_sendbytes(out, relname, relnamelen);

	/* send the attribute info */
	pglogical_write_attrs(out, rel, att_filter);

	/*
	 * Since we've sent the whole relation metadata not just the columns for
	 * the coming row(s), we can omit sending it again. The client will cache
	 * it. If the relation changes the cached flag is cleared by
	 * pglogical_output and we'll be called again next time it's touched.
	 *
	 * We don't care about the cache size here, the size management is done
	 * in the generic cache code.
	 */
	if (cache_entry != NULL)
		cache_entry->is_cached = true;

	pfree(nspname);
}

/*
 * Write relation attributes to the outputstream.
 */
static void
pglogical_write_attrs(StringInfo out, Relation rel, Bitmapset *att_filter)
{
	TupleDesc	desc;
	int			i;
	uint16		nliveatts = 0;
	Bitmapset  *idattrs;

	desc = RelationGetDescr(rel);

	pq_sendbyte(out, 'A');			/* sending ATTRS */

	/* send number of live attributes */
	for (i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = desc->attrs[i];

		if (att->attisdropped)
			continue;
		if (att_filter &&
			!bms_is_member(att->attnum - FirstLowInvalidHeapAttributeNumber,
						   att_filter))
			continue;
		nliveatts++;
	}
	pq_sendint(out, nliveatts, 2);

	/* fetch bitmap of REPLICATION IDENTITY attributes */
	idattrs = RelationGetIndexAttrBitmap(rel, INDEX_ATTR_BITMAP_IDENTITY_KEY);

	/* send the attributes */
	for (i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = desc->attrs[i];
		uint8			flags = 0;
		uint16			len;
		const char	   *attname;

		if (att->attisdropped)
			continue;
		if (att_filter &&
			!bms_is_member(att->attnum - FirstLowInvalidHeapAttributeNumber,
						   att_filter))
			continue;

		if (bms_is_member(att->attnum - FirstLowInvalidHeapAttributeNumber,
						  idattrs))
			flags |= IS_REPLICA_IDENTITY;

		pq_sendbyte(out, 'C');		/* column definition follows */
		pq_sendbyte(out, flags);

		pq_sendbyte(out, 'N');		/* column name block follows */
		attname = NameStr(att->attname);
		len = strlen(attname) + 1;
		pq_sendint(out, len, 2);
		pq_sendbytes(out, attname, len); /* data */
	}

	bms_free(idattrs);
}

/*
 * Write BEGIN to the output stream.
 */
void
pglogical_write_begin(StringInfo out, PGLogicalOutputData *data,
					  ReorderBufferTXN *txn)
{
	uint8	flags = 0;

	pq_sendbyte(out, 'B');		/* BEGIN */

	/* send the flags field its self */
	pq_sendbyte(out, flags);

	/* fixed fields */
	pq_sendint64(out, txn->final_lsn);
	pq_sendint64(out, txn->commit_time);
	pq_sendint(out, txn->xid, 4);
}

/*
 * Write COMMIT to the output stream.
 */
void
pglogical_write_commit(StringInfo out, PGLogicalOutputData *data,
					   ReorderBufferTXN *txn, XLogRecPtr commit_lsn)
{
	uint8 flags = 0;

	pq_sendbyte(out, 'C');		/* sending COMMIT */

	/* send the flags field */
	pq_sendbyte(out, flags);

	/* send fixed fields */
	pq_sendint64(out, commit_lsn);
	pq_sendint64(out, txn->end_lsn);
	pq_sendint64(out, txn->commit_time);
}

/*
 * Write ORIGIN to the output stream.
 */
void
pglogical_write_origin(StringInfo out, const char *origin,
						XLogRecPtr origin_lsn)
{
	uint8	flags = 0;
	uint8	len;

	Assert(strlen(origin) < 255);

	pq_sendbyte(out, 'O');		/* ORIGIN */

	/* send the flags field its self */
	pq_sendbyte(out, flags);

	/* fixed fields */
	pq_sendint64(out, origin_lsn);

	/* origin */
	len = strlen(origin) + 1;
	pq_sendbyte(out, len);
	pq_sendbytes(out, origin, len);
}

/*
 * Write INSERT to the output stream.
 */
void
pglogical_write_insert(StringInfo out, PGLogicalOutputData *data,
						Relation rel, HeapTuple newtuple,
						Bitmapset *att_filter)
{
	uint8 flags = 0;

	pq_sendbyte(out, 'I');		/* action INSERT */

	/* send the flags field */
	pq_sendbyte(out, flags);

	/* use Oid as relation identifier */
	pq_sendint(out, RelationGetRelid(rel), 4);

	pq_sendbyte(out, 'N');		/* new tuple follows */
	pglogical_write_tuple(out, data, rel, newtuple, att_filter);
}

/*
 * Write UPDATE to the output stream.
 */
void
pglogical_write_update(StringInfo out, PGLogicalOutputData *data,
						Relation rel, HeapTuple oldtuple, HeapTuple newtuple,
						Bitmapset *att_filter)
{
	uint8 flags = 0;

	pq_sendbyte(out, 'U');		/* action UPDATE */

	/* send the flags field */
	pq_sendbyte(out, flags);

	/* use Oid as relation identifier */
	pq_sendint(out, RelationGetRelid(rel), 4);

	/*
	 * TODO: support whole tuple (O tuple type)
	 *
	 * Right now we can only write the key-part since logical decoding
	 * doesn't know how to record the whole old tuple for us in WAL.
	 * We can't use REPLICA IDENTITY FULL for this, since that makes
	 * the key-part the whole tuple, causing issues with conflict
	 * resultion and index lookups. We need a separate decoding option
	 * to record whole tuples.
	 */
	if (oldtuple != NULL)
	{
		pq_sendbyte(out, 'K');	/* old key follows */
		pglogical_write_tuple(out, data, rel, oldtuple, att_filter);
	}

	pq_sendbyte(out, 'N');		/* new tuple follows */
	pglogical_write_tuple(out, data, rel, newtuple, att_filter);
}

/*
 * Write DELETE to the output stream.
 */
void
pglogical_write_delete(StringInfo out, PGLogicalOutputData *data,
						Relation rel, HeapTuple oldtuple,
						Bitmapset *att_filter)
{
	uint8 flags = 0;

	pq_sendbyte(out, 'D');		/* action DELETE */

	/* send the flags field */
	pq_sendbyte(out, flags);

	/* use Oid as relation identifier */
	pq_sendint(out, RelationGetRelid(rel), 4);

	/*
	 * TODO support whole tuple ('O' tuple type)
	 *
	 * See notes on update for details
	 */
	pq_sendbyte(out, 'K');	/* old key follows */
	pglogical_write_tuple(out, data, rel, oldtuple, att_filter);
}

/*
 * Most of the brains for startup message creation lives in
 * pglogical_config.c, so this presently just sends the set of key/value pairs.
 */
void
write_startup_message(StringInfo out, List *msg)
{
	ListCell *lc;

	pq_sendbyte(out, 'S');	/* message type field */
	pq_sendbyte(out, PGLOGICAL_STARTUP_MSG_FORMAT_FLAT); 	/* startup message version */
	foreach (lc, msg)
	{
		DefElem *param = (DefElem*)lfirst(lc);
		Assert(IsA(param->arg, String) && strVal(param->arg) != NULL);
		/* null-terminated key and value pairs, in client_encoding */
		pq_sendstring(out, param->defname);
		pq_sendstring(out, strVal(param->arg));
	}
}

/*
 * Write a tuple to the outputstream, in the most efficient format possible.
 */
static void
pglogical_write_tuple(StringInfo out, PGLogicalOutputData *data,
					  Relation rel, HeapTuple tuple, Bitmapset *att_filter)
{
	TupleDesc	desc;
	Datum		values[MaxTupleAttributeNumber];
	bool		isnull[MaxTupleAttributeNumber];
	int			i;
	uint16		nliveatts = 0;

	desc = RelationGetDescr(rel);

	pq_sendbyte(out, 'T');			/* sending TUPLE */

	for (i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = desc->attrs[i];

		if (att->attisdropped)
			continue;
		if (att_filter &&
			!bms_is_member(att->attnum - FirstLowInvalidHeapAttributeNumber,
						   att_filter))
			continue;
		nliveatts++;
	}
	pq_sendint(out, nliveatts, 2);

	/* try to allocate enough memory from the get go */
	enlargeStringInfo(out, tuple->t_len +
					  nliveatts * (1 + 4));

	/*
	 * XXX: should this prove to be a relevant bottleneck, it might be
	 * interesting to inline heap_deform_tuple() here, we don't actually need
	 * the information in the form we get from it.
	 */
	heap_deform_tuple(tuple, desc, values, isnull);

	for (i = 0; i < desc->natts; i++)
	{
		HeapTuple	typtup;
		Form_pg_type typclass;
		Form_pg_attribute att = desc->attrs[i];
		char		transfer_type;

		/* skip dropped columns */
		if (att->attisdropped)
			continue;
		if (att_filter &&
			!bms_is_member(att->attnum - FirstLowInvalidHeapAttributeNumber,
						   att_filter))
			continue;

		if (isnull[i])
		{
			pq_sendbyte(out, 'n');	/* null column */
			continue;
		}
		else if (att->attlen == -1 && VARATT_IS_EXTERNAL_ONDISK(values[i]))
		{
			pq_sendbyte(out, 'u');	/* unchanged toast column */
			continue;
		}

		typtup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(att->atttypid));
		if (!HeapTupleIsValid(typtup))
			elog(ERROR, "cache lookup failed for type %u", att->atttypid);
		typclass = (Form_pg_type) GETSTRUCT(typtup);

		transfer_type = decide_datum_transfer(att, typclass,
											  data->allow_internal_basetypes,
											  data->allow_binary_basetypes);

		switch (transfer_type)
		{
			case 'i':
				pq_sendbyte(out, 'i');	/* internal-format binary data follows */

				/* pass by value */
				if (att->attbyval)
				{
					pq_sendint(out, att->attlen, 4); /* length */

					enlargeStringInfo(out, att->attlen);
					store_att_byval(out->data + out->len, values[i],
									att->attlen);
					out->len += att->attlen;
					out->data[out->len] = '\0';
				}
				/* fixed length non-varlena pass-by-reference type */
				else if (att->attlen > 0)
				{
					pq_sendint(out, att->attlen, 4); /* length */

					appendBinaryStringInfo(out, DatumGetPointer(values[i]),
										   att->attlen);
				}
				/* varlena type */
				else if (att->attlen == -1)
				{
					char *data = DatumGetPointer(values[i]);

					/* send indirect datums inline */
					if (VARATT_IS_EXTERNAL_INDIRECT(values[i]))
					{
						struct varatt_indirect redirect;
						VARATT_EXTERNAL_GET_POINTER(redirect, data);
						data = (char *) redirect.pointer;
					}

					Assert(!VARATT_IS_EXTERNAL(data));

					pq_sendint(out, VARSIZE_ANY(data), 4); /* length */

					appendBinaryStringInfo(out, data, VARSIZE_ANY(data));
				}
				else
					elog(ERROR, "unsupported tuple type");

				break;

			case 'b':
				{
					bytea	   *outputbytes;
					int			len;

					pq_sendbyte(out, 'b');	/* binary send/recv data follows */

					outputbytes = OidSendFunctionCall(typclass->typsend,
													  values[i]);

					len = VARSIZE(outputbytes) - VARHDRSZ;
					pq_sendint(out, len, 4); /* length */
					pq_sendbytes(out, VARDATA(outputbytes), len); /* data */
					pfree(outputbytes);
				}
				break;

			default:
				{
					char   	   *outputstr;
					int			len;

					pq_sendbyte(out, 't');	/* 'text' data follows */

					outputstr =	OidOutputFunctionCall(typclass->typoutput,
													  values[i]);
					len = strlen(outputstr) + 1;
					pq_sendint(out, len, 4); /* length */
					appendBinaryStringInfo(out, outputstr, len); /* data */
					pfree(outputstr);
				}
		}

		ReleaseSysCache(typtup);
	}
}

/*
 * Make the executive decision about which protocol to use.
 */
static char
decide_datum_transfer(Form_pg_attribute att, Form_pg_type typclass,
					  bool allow_internal_basetypes,
					  bool allow_binary_basetypes)
{
	/*
	 * Use the binary protocol, if allowed, for builtin & plain datatypes.
	 */
	if (allow_internal_basetypes &&
		typclass->typtype == 'b' &&
		att->atttypid < FirstNormalObjectId &&
		typclass->typelem == InvalidOid)
	{
		return 'i';
	}
	/*
	 * Use send/recv, if allowed, if the type is plain or builtin.
	 *
	 * XXX: we can't use send/recv for array or composite types for now due to
	 * the embedded oids.
	 */
	else if (allow_binary_basetypes &&
			 OidIsValid(typclass->typreceive) &&
			 (att->atttypid < FirstNormalObjectId || typclass->typtype != 'c') &&
			 (att->atttypid < FirstNormalObjectId || typclass->typelem == InvalidOid))
	{
		return 'b';
	}

	return 't';
}
