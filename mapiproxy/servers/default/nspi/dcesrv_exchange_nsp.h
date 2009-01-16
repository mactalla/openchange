/*
   MAPI Proxy - Exchange NSPI Server

   OpenChange Project

   Copyright (C) Julien Kerihuel 2009

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef	__DCESRV_EXCHANGE_NSP_H
#define	__DCESRV_EXCHANGE_NSP_H

#include <libmapi/libmapi.h>
#include <libmapi/proto_private.h>
#include <mapiproxy/libmapiproxy.h>
#include <ldb.h>
#include <ldb_errors.h>
#include <fcntl.h>
#include <util/debug.h>

#ifndef	__BEGIN_DECLS
#ifdef	__cplusplus
#define	__BEGIN_DECLS		extern "C" {
#define	__END_DECLS		}
#else
#define	__BEGIN_DECLS
#define	__END_DECLS
#endif
#endif

struct emsabp_context {
	struct loadparm_context	*lp_ctx;
	void			*conf_ctx;
	void			*users_ctx;
	void			*ldb_ctx;
	TDB_CONTEXT		*tdb_ctx;
	TDB_CONTEXT		*ttdb_ctx;
	TALLOC_CTX		*mem_ctx;
};


struct exchange_nsp_session {
	struct mpm_session		*session;
	struct exchange_nsp_session	*prev;
	struct exchange_nsp_session	*next;
};


struct emsabp_MId {
	uint32_t	MId;
	char		*dn;
};


/**
   Represents the NSPI Protocol in Permanent Entry IDs.
 */
static const uint8_t GUID_NSPI[] = {
0xDC, 0xA7, 0x40, 0xC8, 0xC0, 0x42, 0x10, 0x1A, 0xB4, 0xB9,
0x08, 0x00, 0x2B, 0x2F, 0xE1, 0x82
};


/**
   PermanentEntryID structure 
 */
struct PermanentEntryID {
	uint8_t			ID_type;	/* constant: 0x0	*/
	uint8_t			R1;		/* reserved: 0x0	*/
	uint8_t			R2;		/* reserved: 0x0	*/
	uint8_t			R3;		/* reserved: 0x0	*/
	struct FlatUID_r      	ProviderUID;	/* constant: GUID_NSPI	*/
	uint32_t		R4;		/* constant: 0x1	*/
	uint32_t		DisplayType;	/* must match one of the existing Display Type value */
	char			*dn;		/* DN string representing the object GUID */
};


/**
   EphemeralEntryID structure
 */
struct EphemeralEntryID {
	uint8_t			ID_type;	/* constant: 0x87	*/
	uint8_t			R1;		/* reserved: 0x0	*/
	uint8_t			R2;		/* reserved: 0x0	*/
	uint8_t			R3;		/* reserved: 0x0	*/
	struct FlatUID_r	ProviderUID;	/* NSPI server GUID	*/
	uint32_t		R4;		/* constant: 0x1	*/
	uint32_t		DisplayType;	/* must match one of the existing Display Type value */
	uint32_t		MId;		/* MId of this object	*/
};

#define	EMSABP_DN	"/guid=%08X%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X"
#define	EMSABP_ADDRTYPE	"EX"

/**
   NSPI PR_CONTAINER_FLAGS values
 */
#define	AB_RECIPIENTS		0x1
#define	AB_SUBCONTAINERS	0x2
#define	AB_UNMODIFIABLE		0x8

#define	EMSABP_TDB_MID_START		0x1b28
#define	EMSABP_TDB_TMP_MID_START	0x5000
#define	EMSABP_TDB_DATA_REC		"MId_index"

__BEGIN_DECLS

NTSTATUS	samba_init_module(void);

/* definitions from emsabp.c */
struct emsabp_context	*emsabp_init(struct loadparm_context *, TDB_CONTEXT *);
bool			emsabp_destructor(void *);
bool			emsabp_verify_user(struct dcesrv_call_state *, struct emsabp_context *);
bool			emsabp_verify_codepage(struct emsabp_context *, uint32_t);
bool			emsabp_verify_lcid(struct emsabp_context *, uint32_t);
struct GUID		*emsabp_get_server_GUID(struct emsabp_context *);
enum MAPISTATUS		emsabp_set_EphemeralEntryID(struct emsabp_context *, uint32_t, uint32_t, struct EphemeralEntryID *);
enum MAPISTATUS		emsabp_set_PermanentEntryID(struct emsabp_context *, uint32_t, struct ldb_message *, struct PermanentEntryID *);
enum MAPISTATUS		emsabp_EphemeralEntryID_to_Binary_r(TALLOC_CTX *, struct EphemeralEntryID *, struct Binary_r *);
enum MAPISTATUS		emsabp_PermanentEntryID_to_Binary_r(TALLOC_CTX *, struct PermanentEntryID *, struct Binary_r *);
enum MAPISTATUS		emsabp_get_HierarchyTable(TALLOC_CTX *, struct emsabp_context *, uint32_t, struct SRowSet **);
enum MAPISTATUS		emsabp_get_CreationTemplatesTable(TALLOC_CTX *, struct emsabp_context *, uint32_t, struct SRowSet **);
void			*emsabp_query(TALLOC_CTX *, struct emsabp_context *, struct ldb_message *, uint32_t, uint32_t);
enum MAPISTATUS		emsabp_fetch_attrs(TALLOC_CTX *, struct emsabp_context *, struct SRow *, uint32_t, struct SPropTagArray *);
enum MAPISTATUS		emsabp_table_fetch_attrs(TALLOC_CTX *, struct emsabp_context *, struct SRow *, uint32_t, struct PermanentEntryID *, 
						 struct PermanentEntryID *, struct ldb_message *, bool);
enum MAPISTATUS		emsabp_search(TALLOC_CTX *, struct emsabp_context *, struct SPropTagArray *, struct Restriction_r *, struct STAT *, uint32_t);
enum MAPISTATUS		emsabp_search_dn(struct emsabp_context *, const char *, struct ldb_message **);
enum MAPISTATUS		emsabp_search_legacyExchangeDN(struct emsabp_context *, const char *, struct ldb_message **);

/* definitions from emsabp_tdb.c */
TDB_CONTEXT		*emsabp_tdb_init(TALLOC_CTX *, struct loadparm_context *);
enum MAPISTATUS		emsabp_tdb_close(TDB_CONTEXT *);
enum MAPISTATUS		emsabp_tdb_fetch(TDB_CONTEXT *, const char *, TDB_DATA *);
enum MAPISTATUS		emsabp_tdb_insert(TDB_CONTEXT *, const char *);
enum MAPISTATUS		emsabp_tdb_fetch_MId(TDB_CONTEXT *, const char *, uint32_t *);
bool			emsabp_tdb_lookup_MId(TDB_CONTEXT *, uint32_t);
enum MAPISTATUS		emsabp_tdb_fetch_dn_from_MId(TALLOC_CTX *, TDB_CONTEXT *, uint32_t, char **);

TDB_CONTEXT		*emsabp_tdb_init_tmp(TALLOC_CTX *);

/* definitions from emsabp_property.c */
const char		*emsabp_property_get_attribute(uint32_t);
uint32_t		emsabp_property_get_ulPropTag(const char *);
int			emsabp_property_is_ref(uint32_t);
const char		*emsabp_property_get_ref_attr(uint32_t);

__END_DECLS

#endif /* __DCESRV_EXCHANGE_NSP_H */