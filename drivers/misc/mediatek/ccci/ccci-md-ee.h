/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CCCI_MD_EE_H
#define __CCCI_MD_EE_H

#include <linux/types.h>

/* Bounded v5 exception-overview decode. No payload beyond the header fields. */
#define CCCI_MD_EE_MAX_CORES	16
#define CCCI_MD_EE_TEXT		64
#define CCCI_MD_EE_FILE		96

struct ccci_md_ee {
	u32 version;
	u32 cores;
	u16 ex_type;
	u8 content_type;	/* 0 assert, 1 fatal, 2 custom */
	u8 section;
	u8 cadefa;
	u8 filename_supported;
	char core[12];
	/* fatal */
	u32 code[3];
	u32 status;
	u32 sp;
	u32 pc;
	u32 lr;
	u32 addr;
	u32 cause;
	char file[CCCI_MD_EE_FILE];
	/* assert */
	u32 line;
	u32 para[3];
	char path[CCCI_MD_EE_TEXT];
};

/* Bytes needed to decode either class from a record with this core count. */
size_t ccci_md_ee_size(u32 cores);

int ccci_md_ee_decode(const u8 *data, size_t bytes, struct ccci_md_ee *out);

#endif
