/*
 *  OpenChange MAPI implementation.
 *
 *  Copyright (C) Julien Kerihuel 2005 - 2007.
 *  Copyright (C) Jelmer Vernooij 2005.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <libmapi/libmapi.h>
#include <libmapi/proto_private.h>
#include <gen_ndr/ndr_exchange.h>
#include <gen_ndr/ndr_exchange_c.h>

#include <core/nterr.h>
#include <param.h>
#include <credentials.h>

#define	ECDOCONNECT_FORMAT	"/o=%s/ou=%s/cn=Recipients/cn=%s"

/**
 * Create a connection on the exchange_emsmdb pipe
 */

struct emsmdb_context *emsmdb_connect(TALLOC_CTX *mem_ctx, struct dcerpc_pipe *p, struct cli_credentials *cred)
{
	struct EcDoConnect	r;
	NTSTATUS		status;
	struct emsmdb_context	*ret;
	mapi_ctx_t		*mapi_ctx;

	mapi_ctx = global_mapi_ctx;
	if (mapi_ctx == 0) return NULL;

	ret = talloc_zero(mem_ctx, struct emsmdb_context);
	ret->rpc_connection = p;
	ret->mem_ctx = mem_ctx;

	ret->cache_requests = talloc(mem_ctx, struct EcDoRpc_MAPI_REQ *);

	r.in.name = mapi_ctx->session->profile->mailbox;
	r.in.unknown1[0] = 0x0;
	r.in.unknown1[1] = 0x1eeebaac;
	r.in.unknown1[2] = 0x0;
	r.in.code_page = mapi_ctx->session->profile->codepage;
	r.in.input_locale.language = mapi_ctx->session->profile->language;
	r.in.input_locale.method = mapi_ctx->session->profile->method;
	r.in.unknown2 = 0xffffffff;
	r.in.unknown3 = 0x1;
	r.in.emsmdb_client_version[0] = 0x000a;
	r.in.emsmdb_client_version[1] = 0x0000;
	r.in.emsmdb_client_version[2] = 0x1013;

	r.in.alloc_space = talloc(mem_ctx, uint32_t);
	*r.in.alloc_space = 0;

	r.out.unknown4[0] = (uint32_t)talloc(mem_ctx, uint32_t);
	r.out.unknown4[1] = (uint32_t)talloc(mem_ctx, uint32_t);
	r.out.unknown4[2] = (uint32_t)talloc(mem_ctx, uint32_t);
	r.out.session_nb = talloc(mem_ctx, uint16_t);
	r.out.alloc_space = talloc(mem_ctx, uint32_t);
	r.out.handle = &ret->handle;

	status = dcerpc_EcDoConnect(p, mem_ctx, &r);

	if (!MAPI_STATUS_IS_OK(NT_STATUS_V(status))) {
		mapi_errstr("EcDoConnect", r.out.result);
		return NULL;
	}

	DEBUG(3, ("emsmdb_connect\n"));
	DEBUG(3, ("\t\t user = %s\n", r.out.user));
	DEBUG(3, ("\t\t organization = %s\n", r.out.org_group));
	
	ret->cred = cred;
	ret->max_data = 0xFFF0;
	ret->setup = False;
	
	return ret;
}

/**
 * Destructor
 */

int emsmdb_disconnect_dtor(void *data)
{
	NTSTATUS		status;
	struct mapi_provider	*provider = (struct mapi_provider *)data;

	status = emsmdb_disconnect(provider->ctx);	
	return 0;
}

/**
 * Close the connection on the initialized exchange_emsmdb pipe
 */

NTSTATUS emsmdb_disconnect(struct emsmdb_context *emsmdb)
{
	NTSTATUS		status;
	struct EcDoDisconnect	r;

	r.in.handle = &emsmdb->handle;

	/* fixme: dcerpc_EcDoDisconnect bug */
	status = NT_STATUS_OK;
/* 	status = dcerpc_EcDoDisconnect(emsmdb->rpc_connection, emsmdb, &r); */
/* 	if (!MAPI_STATUS_IS_OK(NT_STATUS_V(status))) { */
/* 		mapi_errstr("EcDoDisconnect", r.out.result); */
/* 		return status; */
/* 	} */

	return NT_STATUS_OK;
}

NTSTATUS emsmdb_transaction(struct emsmdb_context *emsmdb, struct mapi_request *req, struct mapi_response **repl)
{
	struct EcDoRpc		r;
	struct mapi_response	*mapi_response;
	uint16_t		*length;
	NTSTATUS		status;
	struct EcDoRpc_MAPI_REQ	*multi_req;
	uint8_t			i = 0;

start:
	r.in.handle = r.out.handle = &emsmdb->handle;
	r.in.size = emsmdb->max_data;
	r.in.offset = 0x0;

	mapi_response = talloc_zero(emsmdb->mem_ctx, struct mapi_response);	
	r.out.mapi_response = mapi_response;

	/* process cached data */
	if (emsmdb->cache_count) {
		multi_req = talloc_array(emsmdb->mem_ctx, struct EcDoRpc_MAPI_REQ, emsmdb->cache_count + 2);
		for (i = 0; i < emsmdb->cache_count; i++) {
			multi_req[i] = *emsmdb->cache_requests[i];
		}
		multi_req[i] = req->mapi_req[0];
		req->mapi_req = multi_req;
	}

	req->mapi_req = talloc_realloc(emsmdb->mem_ctx, req->mapi_req, struct EcDoRpc_MAPI_REQ, emsmdb->cache_count + 2);
	req->mapi_req[i+1].opnum = 0;

	r.in.mapi_request = req;
	r.in.mapi_request->mapi_len += emsmdb->cache_size;
	r.in.mapi_request->length += emsmdb->cache_size;
	length = talloc_zero(emsmdb->mem_ctx, uint16_t);
	*length = r.in.mapi_request->mapi_len;
	r.in.length = r.out.length = length;
	r.in.max_data = (*length >= 0x4000) ? 0x7FFF : emsmdb->max_data;

	status = dcerpc_EcDoRpc(emsmdb->rpc_connection, emsmdb->mem_ctx, &r);

	if (!MAPI_STATUS_IS_OK(NT_STATUS_V(status))) {
		if (emsmdb->setup == False) {
			errno = 0;
			emsmdb->max_data = 0x7FFF;
			emsmdb->setup = True;
			goto start;
		} else {
			return status;
		}
	} else {
		emsmdb->setup = True;
	}
	emsmdb->cache_size = emsmdb->cache_count = 0;

	*repl = r.out.mapi_response;

	return status;
}


/* fixme: move to right place */
#include <string.h>
#include <netinet/in.h>
NTSTATUS dcerpc_EcRRegisterPushNotification(struct dcerpc_pipe *p, TALLOC_CTX *mem_ctx, struct EcRRegisterPushNotification *r);

_PUBLIC_ NTSTATUS emsmdb_register_notification(const struct sockaddr_in* addr)
{
	struct EcRRegisterPushNotification request;
	NTSTATUS		status;
	mapi_ctx_t		*mapi_ctx;
	struct emsmdb_context	*emsmdb;
	struct policy_handle handle;
	TALLOC_CTX* mem;

	mem = talloc_init("local");
	mapi_ctx = global_mapi_ctx;
	emsmdb = mapi_ctx->session->emsmdb->ctx;

	/* in */
	request.in.handle = &emsmdb->handle;
	request.in.unknown1 = 0x00000000;
	request.in.len = 0x00000008;

	memcpy(request.in.payload, "\xe8\x57\xff\x00\x00\x00\x00\x00", sizeof(request.in.payload));

	/* post payload */
	request.in.unknown2 = 0x0008;
	request.in.unknown3 = 0x0008; /* varies */
	request.in.unknown4 = 0xffffffff;
	request.in.unknown5 = 0x00000010;
	request.in.unknown6 = 0x0002;

	/* addressing */
	request.in.port = addr->sin_port;
	*((unsigned long*)&request.in.address) = addr->sin_addr.s_addr;

	/* post addressing, does not vary */
	memcpy(request.in.unknown7, "\x00\x00\x00\x00\x00\x00\x00\x00\x10\x00", sizeof(request.in.unknown7));

	/* out */
	request.out.handle = &handle;

	status = dcerpc_EcRRegisterPushNotification(emsmdb->rpc_connection, mem, &request);
	talloc_free(mem);

	if (request.out.result != MAPI_E_SUCCESS) {
		return NT_STATUS_UNSUCCESSFUL;
	}
	return NT_STATUS_OK;
}


void *pull_emsmdb_property(TALLOC_CTX *mem_ctx, uint32_t *offset, enum MAPITAGS tag, DATA_BLOB *data)
{
	struct ndr_pull		*ndr;
	const char		*pt_string8;
	uint16_t		*pt_i2;
	uint64_t		*pt_i8;
	uint32_t		*pt_long;
	uint16_t		*pt_boolean;
	struct FILETIME		*pt_filetime;
	struct SBinary_short	pt_binary;
	struct SBinary		*sbin;

	ndr = talloc_zero(mem_ctx, struct ndr_pull);
	ndr->offset = *offset;
	ndr->data = data->data;
	ndr->data_size = data->length;
	ndr_set_flags(&ndr->flags, LIBNDR_FLAG_NOALIGN);

	switch(tag & 0xFFFF) {
	case PT_BOOLEAN:
		pt_boolean = talloc_zero(mem_ctx, uint16_t);
		ndr_pull_uint16(ndr, NDR_SCALARS, pt_boolean);
		*offset = ndr->offset;
		return (void *) pt_boolean;
	case PT_I2:
		pt_i2 = talloc_zero(mem_ctx, uint16_t);
		ndr_pull_uint16(ndr, NDR_SCALARS, pt_i2);
		*offset = ndr->offset;
		return (void *) pt_i2;
	case PT_NULL:
	case PT_ERROR:
	case PT_LONG:
		pt_long = talloc_zero(mem_ctx, uint32_t);
		ndr_pull_uint32(ndr, NDR_SCALARS, pt_long);
		*offset = ndr->offset;
		return (void *) pt_long;
	case PT_I8:
		pt_i8 = talloc_zero(mem_ctx, uint64_t);
		ndr_pull_hyper(ndr, NDR_SCALARS, pt_i8);
		*offset = ndr->offset;
		return (void *) pt_i8;
	case PT_SYSTIME:
		pt_filetime = talloc_zero(mem_ctx, struct FILETIME);
		ndr_pull_hyper(ndr, NDR_SCALARS, (uint64_t*)pt_filetime);
		*offset = ndr->offset;
		return (void*) pt_filetime;
	case PT_UNICODE:
	case PT_STRING8:
		ndr_set_flags(&ndr->flags, LIBNDR_FLAG_STR_ASCII|LIBNDR_FLAG_STR_NULLTERM);
		ndr_pull_string(ndr, NDR_SCALARS, &pt_string8);
		*offset = ndr->offset;
		return (void *) pt_string8;
	case PT_OBJECT:
	case PT_BINARY:
		ndr_pull_SBinary_short(ndr, NDR_SCALARS, &pt_binary);
		*offset = ndr->offset;
		sbin = talloc_zero(mem_ctx, struct SBinary);
		sbin->cb = pt_binary.cb;
		sbin->lpb = pt_binary.lpb;
		return (void *) sbin;
	default:
		return NULL;
	}	
}

enum MAPISTATUS emsmdb_get_SPropValue(TALLOC_CTX *mem_ctx,
				      DATA_BLOB *content,
				      struct SPropTagArray *tags,
				      struct SPropValue **propvals, uint32_t *cn_propvals,
				      uint8_t layout)
{
	struct SPropValue	*p_propval;
	uint32_t		i_propval;
	uint32_t		i_tag;
	uint32_t		cn_tags;
	uint32_t		offset = 0;
	void			*data;

	i_propval = 0;
	cn_tags = tags->cValues;
	*cn_propvals = 0;
	*propvals = talloc_array(mem_ctx, struct SPropValue, cn_tags);

	for (i_tag = 0; i_tag < cn_tags; i_tag++) {
		if (layout) { 
			if (((uint8_t)(*(content->data + offset))) == PT_ERROR) {
				tags->aulPropTag[i_tag] &= 0xFFFF0000;
				tags->aulPropTag[i_tag] |= PT_ERROR;
			}
			offset += sizeof (uint8_t);
		}

		data = pull_emsmdb_property(mem_ctx, &offset, tags->aulPropTag[i_tag], content);
		if (data) {
			p_propval = &((*propvals)[i_propval]);
			p_propval->ulPropTag = tags->aulPropTag[i_tag];
			p_propval->dwAlignPad = 0x0;
			set_SPropValue(p_propval, data);
			i_propval++;
		}
	}

	*cn_propvals = i_propval;
	return MAPI_E_SUCCESS;
}

void	emsmdb_get_SRowSet(TALLOC_CTX *mem_ctx, struct SRowSet *rowset, struct SPropTagArray *proptags, DATA_BLOB *content, uint8_t layout, uint8_t align)
{
	struct SRow	*rows;
	struct SPropValue *lpProps;
	uint32_t	idx;
	uint32_t	prop;
	uint32_t	offset = 0;
	void		*data;
	uint32_t	row_count;

	/* caller allocated */
	rows = rowset->aRow;
	row_count = rowset->cRows;

	for (idx = 0; idx < row_count; idx++) {
		lpProps = talloc_array(mem_ctx, struct SPropValue, proptags->cValues);
		for (prop = 0; prop < proptags->cValues; prop++) {
			if (layout) {
				  if (((uint8_t)(*(content->data + offset))) == PT_ERROR) {
					proptags->aulPropTag[prop] &= 0xFFFF0000;
					proptags->aulPropTag[prop] |= 0xA;
				}
				offset += align;
			}
			data = pull_emsmdb_property(mem_ctx, &offset, proptags->aulPropTag[prop], content);
			lpProps[prop].ulPropTag = proptags->aulPropTag[prop];
			lpProps[prop].dwAlignPad = 0x0;
			set_SPropValue(&lpProps[prop], data);
		}
		if (align) {
			offset += align;
		}

		rows[idx].ulAdrEntryPad = 0;
		rows[idx].cValues = proptags->cValues;
		rows[idx].lpProps = lpProps;
	}
}
