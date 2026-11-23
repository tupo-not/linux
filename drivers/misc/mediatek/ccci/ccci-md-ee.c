// SPDX-License-Identifier: GPL-2.0-only
/*
 * Bounded decoder for the v5 MD exception overview header.
 *
 * Layout is taken from the same-board 4.19 mdee_dumper_v5.h:
 *   struct ex_overview_t {
 *       u32 overview_verno;
 *       u32 core_num;
 *       struct { char core_name[11]; u8 is_offender; } main_reson[core_num];
 *       struct ex_brief_maininfo_v5 ex_info;   // ex_type u16, format, class, ...
 *       union { struct ex_fatal_v5; struct ex_assert_v5; } info;
 *   }
 * core_num is a record field, not a constant: the same board reports 12 cores,
 * so every later offset depends on it. Unknown core counts fail closed.
 */
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "ccci-md-ee.h"

#define EE_CORE_RECORD	12
#define EE_INFO		8	/* overview_verno + core_num */
#define EE_CLASS_FATAL	1
#define EE_FATAL_BYTES	0x40	/* through error_cause */
#define EE_FATAL_FILE	0x34	/* sizeof(struct ex_fatal_v5) */
#define EE_ASSERT_BYTES	0x11c	/* through lr */

size_t ccci_md_ee_size(u32 cores)
{
	if (!cores || cores > CCCI_MD_EE_MAX_CORES)
		return 0;
	return EE_INFO + cores * EE_CORE_RECORD + EE_ASSERT_BYTES;
}

int ccci_md_ee_decode(const u8 *data, size_t bytes, struct ccci_md_ee *out)
{
	size_t info, need;
	unsigned int i;

	if (!data || !out)
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	if (bytes < EE_INFO)
		return -ENODATA;
	out->version = get_unaligned_le32(data);
	out->cores = get_unaligned_le32(data + 4);
	info = EE_INFO + (size_t)out->cores * EE_CORE_RECORD;
	need = ccci_md_ee_size(out->cores);
	if (!need || bytes < info + EE_FATAL_BYTES)
		return -ENODATA;
	for (i = 0; i < out->cores; i++) {
		size_t base = EE_INFO + (size_t)i * EE_CORE_RECORD;

		/* First offender wins; fall back to the first named core. */
		if (!data[base + 11])
			continue;
		memcpy(out->core, data + base, sizeof(out->core) - 1);
		break;
	}
	out->ex_type = get_unaligned_le16(data + info);
	out->content_type = data[info + 3];
	if (out->content_type == EE_CLASS_FATAL) {
		size_t off = info + 8 + EE_FATAL_FILE;

		out->cadefa = data[info + 8 + 20];
		out->filename_supported = data[info + 8 + 21];
		out->section = data[info + 8 + 22];
		out->code[0] = get_unaligned_le32(data + info + 8);
		out->code[1] = get_unaligned_le32(data + info + 12);
		out->code[2] = get_unaligned_le32(data + info + 16);
		out->status = get_unaligned_le32(data + info + 8 + 28);
		out->sp = get_unaligned_le32(data + info + 8 + 32);
		out->pc = get_unaligned_le32(data + info + 8 + 36);
		out->lr = get_unaligned_le32(data + info + 8 + 40);
		out->addr = get_unaligned_le32(data + info + 8 + 44);
		out->cause = get_unaligned_le32(data + info + 8 + 48);
		/*
		 * The modem names the offending source file after the fixed
		 * record when it supports it. The string is not length-checked
		 * on that side, so it is bounded by the bytes we hold.
		 */
		if (out->filename_supported == 1) {
			for (i = 0; i < sizeof(out->file) - 1 && off + i < bytes; i++) {
				u8 c = data[off + i];

				if (!c)
					break;
				out->file[i] = (c >= 0x20 && c < 0x7f) ? c : '.';
			}
		}
		return 0;
	}
	if (bytes < info + EE_ASSERT_BYTES)
		return -ENODATA;
	for (i = 0; i < sizeof(out->path) - 1; i++) {
		u8 c = data[info + 8 + i];

		if (!c)
			break;
		out->path[i] = (c >= 0x20 && c < 0x7f) ? c : '.';
	}
	out->line = get_unaligned_le32(data + info + 8 + 256);
	out->para[0] = get_unaligned_le32(data + info + 8 + 260);
	out->para[1] = get_unaligned_le32(data + info + 8 + 264);
	out->para[2] = get_unaligned_le32(data + info + 8 + 268);
	out->lr = get_unaligned_le32(data + info + 8 + 272);
	return 0;
}
