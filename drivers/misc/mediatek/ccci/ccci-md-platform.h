/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CCCI_MD_PLATFORM_H
#define __CCCI_MD_PLATFORM_H

struct platform_device;
struct ccci_smem_layout;

int ccci_md_platform_create(struct platform_device *pdev, const struct ccci_smem_layout *layout);
void ccci_md_platform_shutdown(struct platform_device *pdev);

#endif
