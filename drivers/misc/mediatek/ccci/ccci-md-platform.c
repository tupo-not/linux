// SPDX-License-Identifier: GPL-2.0-only
/* Same-board cold MD platform cycle. Never issue the MD CPU boot SMC. */
#include <linux/arm-smccc.h>
#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/hex.h>
#include <linux/mfd/syscon.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/soc/mediatek/mtk_sip_svc.h>
#include <linux/soc/mediatek/mt6893-md.h>

#include "ccci-md-platform.h"
#include "ccci-ccif.h"
#include "ccci-ccism-bridge.h"
#include "ccci-dpmaif.h"
#include "ccci-md-at.h"
#include "ccci-md-net.h"
#include "ccci-md-boot.h"
#include "ccci-md-ee.h"
#include "ccci-nvram-cache.h"
#include "ccci-rtc32k.h"
#include "ccci-service-endpoint.h"
#include "ccci-port-dispatch.h"
#include "ccci-rpc-op6893.h"
#include "ccci-rpc-x6710.h"
#include "ccci-md-power.h"
#include "ccci-smem.h"
#include "ccci-smem-clear.h"

#define MD_FLIGHT_MODE	7
#define MD_POWER_CONFIG	6
#define MD_CHECK_DONE	3
#define MD_PROTECT2	(BIT(10) | BIT(21) | BIT(29))
#define SPM_PCM_REG13	0x110

struct ccci_md_platform {
	/* Serializes commands, state reporting and shutdown. */
	struct mutex lock;
	struct ccci_md_power power;
	struct ccci_smem_layout smem;
	struct regmap *spm;
	struct regmap *infra;
	struct regulator *rails[4];
	int voltage[4];
	struct clk_bulk_data clocks[4];
	struct device *lookup;
	struct device *domain;
	struct ccci_ccif *ccif;
	struct ccci_dpmaif *dpmaif;
	struct ccci_ccism_bridge *bridge;
	struct ccci_service_endpoint *services;
	struct ccci_port_dispatch *dispatch;
	/* AT/MUX byte pipe, created only with the dispatcher. */
	struct ccci_md_at *at;
	/* CCMNI netdev, created only with the dispatcher. */
	struct ccci_md_net *net;
	struct ccci_nvram_cache *cache;
	struct ccci_md_boot *boot;
	bool dpmaif_off_reset;
	bool dpmaif_preboot_failed;
	bool restore_rails;
	/* Read-only MD exception record header, mapped only when LK exported it. */
	void __iomem *ee;
	size_t ee_bytes;
	/* Same reservation: the MD writes its own log there; text only. */
	void __iomem *dbg;
	size_t dbg_bytes;
	/* MD DFD log region: much larger than the exception record, streamed. */
	void __iomem *dfd;
	size_t dfd_bytes;
};

/* Independent hardware check also catches a genpd power-off veto/failure. */
static int md_domain_check(struct ccci_md_platform *md, bool on)
{
	u32 status, control, iso, protect1, protect2, protect3;
	int ret;

	ret = regmap_read(md->spm, 0x16c, &status);
	if (ret)
		return ret;
	ret = regmap_read(md->spm, 0x300, &control);
	if (ret)
		return ret;
	ret = regmap_read(md->spm, 0x398, &iso);
	if (ret)
		return ret;
	ret = regmap_read(md->infra, 0x228, &protect1);
	if (ret)
		return ret;
	ret = regmap_read(md->infra, 0xb90, &protect2);
	if (ret)
		return ret;
	ret = regmap_read(md->infra, 0xbc4, &protect3);
	if (ret)
		return ret;
	if ((status & BIT(0)) != (on ? BIT(0) : 0) ||
	    (control & (BIT(0) | BIT(2))) != (on ? BIT(0) | BIT(2) : 0) ||
	    (iso & GENMASK(1, 0)) != (on ? 0 : GENMASK(1, 0)) ||
	    (protect1 & BIT(7)) != (on ? 0 : BIT(7)) ||
	    (protect2 & MD_PROTECT2) != (on ? 0 : MD_PROTECT2) ||
	    (protect3 & GENMASK(7, 6)) != (on ? 0 : GENMASK(7, 6)))
		return -EBUSY;
	return 0;
}

static int md_off_check(void *context)
{
	return md_domain_check(context, false);
}

static int md_read_rails(struct ccci_md_platform *md, int voltage[4])
{
	int ret;

	for (unsigned int i = 0; i < ARRAY_SIZE(md->rails); i++) {
		ret = regulator_is_enabled(md->rails[i]);
		if (ret <= 0)
			return ret ?: -ENODATA;
		ret = regulator_get_voltage(md->rails[i]);
		if (ret <= 0)
			return ret ?: -ENODATA;
		voltage[i] = ret;
	}
	return 0;
}

static int md_rails_check(void *context)
{
	struct ccci_md_platform *md = context;
	int voltage[4], ret;

	ret = md_read_rails(md, voltage);
	if (ret)
		return ret;
	if (memcmp(voltage, md->voltage, sizeof(voltage)))
		return -ESTALE;
	return md_domain_check(md, md->power.state == CCCI_MD_INHERITED);
}

/* Only after verified MD1 OFF and a clean transport rearm. */
static int md_restore_rails(struct ccci_md_platform *md)
{
	static const u8 order[] = { 0, 1, 3 };
	int current_uv[ARRAY_SIZE(md->rails)], ret;

	if (md->power.state != CCCI_MD_STOPPED || md->power.domain ||
	    md->power.clocks || md->power.attempted)
		return -EBUSY;
	ret = md_domain_check(md, false);
	if (ret)
		return ret;
	ret = md_read_rails(md, current_uv);
	if (ret)
		return ret;
	/*
	 * Restore the observed board transition only. Keep VSRAM_MD at its
	 * boot setting throughout, with the rising core rails no higher than it.
	 */
	if (current_uv[2] != md->voltage[2] || md->voltage[0] > current_uv[2] ||
	    md->voltage[1] > current_uv[2])
		return -ESTALE;
	for (unsigned int i = 0; i < ARRAY_SIZE(md->rails); i++)
		if (current_uv[i] > md->voltage[i])
			return -ESTALE;
	md->power.step = "restore-rails";
	for (unsigned int i = 0; i < ARRAY_SIZE(order); i++) {
		unsigned int n = order[i];
		int uv = md->voltage[n];

		if (current_uv[n] == uv)
			continue;
		ret = regulator_set_voltage(md->rails[n], uv, uv);
		if (!ret && regulator_get_voltage(md->rails[n]) != uv)
			ret = regulator_sync_voltage(md->rails[n]);
		if (ret)
			return ret;
	}
	usleep_range(2000, 2500);
	return md_rails_check(md);
}

static int md_clocks(void *context, bool on)
{
	struct ccci_md_platform *md = context;

	if (on)
		return clk_bulk_prepare_enable(ARRAY_SIZE(md->clocks), md->clocks);
	clk_bulk_disable_unprepare(ARRAY_SIZE(md->clocks), md->clocks);
	return 0;
}

static int md_srcclk(void *context, bool on)
{
	struct ccci_md_platform *md = context;
	u32 value;
	int ret;

	ret = regmap_update_bits(md->infra, 0xf0c, 0xff, on ? 0x21 : 0);
	if (ret)
		return ret;
	ret = regmap_read(md->infra, 0xf0c, &value);
	if (ret)
		return ret;
	return (value & 0xff) == (on ? 0x21 : 0) ? 0 : -EIO;
}

static int md_srclken_o1(void *context)
{
	struct ccci_md_platform *md = context;
	u32 value;
	int ret;

	ret = regmap_write(md->spm, 0, 0xb160001);
	if (ret)
		return ret;
	ret = regmap_set_bits(md->spm, 8, BIT(21));
	if (ret)
		return ret;
	ret = regmap_read(md->spm, 8, &value);
	if (ret)
		return ret;
	/* Same-board vendor OFF does not clear this shared SPM bit. */
	return value & BIT(21) ? 0 : -EIO;
}

static int md_flight(void *context, bool flight)
{
	struct arm_smccc_res res;

	arm_smccc_smc(MTK_SIP_KERNEL_CCCI_CONTROL, MD_FLIGHT_MODE, flight, 0, 0, 0, 0, 0, &res);
	if (res.a0 == (unsigned long)SMCCC_RET_NOT_SUPPORTED)
		return -EOPNOTSUPP;
	/* No fallback function IDs or interpretation of diagnostic a1..a3. */
	return res.a0 ? -EIO : 0;
}

static int md_domain(void *context, bool on)
{
	struct ccci_md_platform *md = context;
	int ret;

	if (on)
		ret = pm_runtime_resume_and_get(md->domain);
	else
		ret = pm_runtime_put_sync_suspend(md->domain);
	if (ret < 0)
		return ret;
	return md_domain_check(md, on);
}

static int md_handoff_check(void *context)
{
	struct ccci_md_platform *md = context;
	struct arm_smccc_res res;
	int ret;

	ret = md_domain_check(md, true);
	if (ret)
		return ret;
	/* Same-board md_start_platform(): up to 100 CHECK_DONE requests at 20 ms. */
	for (unsigned int i = 0; i < 100; i++) {
		arm_smccc_smc(MTK_SIP_KERNEL_CCCI_CONTROL, MD_POWER_CONFIG, MD_CHECK_DONE,
			      0, 0, 0, 0, 0, &res);
		if (!res.a0)
			return md_domain_check(md, true);
		if (res.a0 == (unsigned long)SMCCC_RET_NOT_SUPPORTED)
			return -EOPNOTSUPP;
		msleep(20);
	}
	return -ETIMEDOUT;
}

static int md_handoff_acquire(void *context)
{
	struct ccci_md_platform *md = context;
	int ret;

	ret = md_domain_check(md, true);
	if (ret)
		return ret;
	ret = pm_runtime_resume_and_get(md->domain);
	if (ret < 0)
		return ret;
	/* Hold the runtime reference before lifting the inherited-domain OFF veto. */
	return mt6893_md_domain_allow_off(md->domain);
}

static const struct ccci_md_power_ops md_power_ops = {
	.off_check = md_off_check,
	.rails_check = md_rails_check,
	.clocks = md_clocks,
	.srcclk = md_srcclk,
	.srclken_o1 = md_srclken_o1,
	.flight = md_flight,
	.domain = md_domain,
	.handoff_check = md_handoff_check,
	.handoff_acquire = md_handoff_acquire,
};

static ssize_t md_power_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	static const char * const states[] = { "cold", "powered", "stopped", "lk-on", "fault" };
	struct ccci_md_platform *md = dev_get_drvdata(dev);
	ssize_t ret;

	mutex_lock(&md->lock);
	ret = sysfs_emit(buf,
			 "state=%s step=%s error=%d attempted=%u clocks=%u domain=%u cpu_release_issued=0 rails_uv=%d,%d,%d,%d smem_users=%u\n",
			 states[md->power.state], md->power.step, md->power.error,
			 md->power.attempted, md->power.clocks, md->power.domain,
			 md->voltage[0], md->voltage[1], md->voltage[2], md->voltage[3],
			 md->smem.count);
	mutex_unlock(&md->lock);
	return ret;
}

/*
 * Active boot: join CCIF while clocks are held, prove MD1 OFF, then stop
 * services and SCP. Reset CCIF at the next prepare, with the MD CPU held.
 */
static int md_platform_stop(struct ccci_md_platform *md)
{
	u32 r13 = 0;
	int ret;

	ret = ccci_dpmaif_quiesce(md->dpmaif);
	if (ret)
		return ret;

	if (ccci_md_boot_active(md->boot)) {
		if (!regmap_read(md->spm, SPM_PCM_REG13, &r13))
			ccci_md_boot_note_pcm(md->boot, r13);
		ccci_ccif_quiesce_rx(md->ccif);
		ccci_service_endpoint_park(md->services);
		ret = ccci_ccif_park(md->ccif);
		if (!ret)
			ret = ccci_md_power_off(&md->power);
		if (!ret) {
			ccci_ccif_powered_off(md->ccif);
			ret = ccci_md_boot_stop(md->boot, true);
		}
		if (!ret) {
			ret = ccci_dpmaif_reset_after_off(md->dpmaif);
			md->dpmaif_off_reset = !ret;
		}
		return ret;
	}
	ret = ccci_md_boot_offline(md->boot);
	if (!ret)
		ret = ccci_nvram_cache_offline(md->cache);
	if (!ret)
		ret = ccci_port_dispatch_offline(md->dispatch);
	if (!ret)
		ret = ccci_service_endpoint_offline(md->services);
	if (!ret)
		ret = ccci_ccism_bridge_offline(md->bridge);
	if (!ret && md->ccif && md->power.state == CCCI_MD_POWERED)
		ret = ccci_ccif_stop(md->ccif);
	if (!ret && md->power.state != CCCI_MD_INHERITED)
		ret = ccci_md_power_off(&md->power);
	if (!ret)
		ret = ccci_nvram_cache_stop(md->cache);
	if (!ret && md->power.state == CCCI_MD_STOPPED) {
		ret = ccci_dpmaif_reset_after_off(md->dpmaif);
		md->dpmaif_off_reset = !ret;
	}
	return ret;
}

static int md_platform_off(struct ccci_md_platform *md)
{
	int ret;

	if (!md->dpmaif_preboot_failed)
		return md_platform_stop(md);

	/* Only recover an attempt that never started the MD power sequence. */
	if (md->power.state != CCCI_MD_STOPPED || md->power.attempted ||
	    md->power.clocks || md->power.domain)
		return -EIO;
	md->dpmaif_off_reset = false;
	ret = md_domain_check(md, false);
	if (ret)
		return ret;
	ret = md_platform_stop(md);
	if (ret)
		return ret;
	if (!md->dpmaif_off_reset)
		return -EIO;
	ret = md_domain_check(md, false);
	if (ret)
		return ret;
	md->dpmaif_preboot_failed = false;
	return 0;
}

static int md_power_on_after_dl(void *data)
{
	struct ccci_md_platform *md = data;

	return ccci_md_power_on(&md->power);
}

static int md_platform_on(struct ccci_md_platform *md, bool prearm_dl)
{
	int ret;

	if (md->dpmaif_preboot_failed)
		return -EIO;
	if (prearm_dl) {
		if (!md->dpmaif || !md->boot)
			return -EOPNOTSUPP;
		/* Require a clean stop, a completed off reset and explicit rearm. */
		if (md->power.state != CCCI_MD_STOPPED || md->power.attempted ||
		    md->power.domain || md->power.clocks || !md->dpmaif_off_reset)
			return -EBUSY;
		ret = md_domain_check(md, false);
		if (ret)
			return ret;
	}
	ret = md->restore_rails ? md_restore_rails(md) : 0;
	if (ret)
		return ret;

	if (prearm_dl) {
		ret = ccci_dpmaif_power_with_dl(md->dpmaif, md_power_on_after_dl, md);
		/* Latch failures so regular power-on cannot bypass this attempt. */
		md->dpmaif_preboot_failed = !!ret;
	} else {
		ret = ccci_md_power_on(&md->power);
		if (!ret)
			ccci_dpmaif_allow_start(md->dpmaif);
	}
	if (!ret) {
		md->restore_rails = false;
		md->dpmaif_off_reset = false;
	}
	return ret;
}

static ssize_t md_power_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct ccci_md_platform *md = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&md->lock);
	if (sysfs_streq(buf, "on")) {
		ret = md_platform_on(md, false);
	} else if (sysfs_streq(buf, "on-with-dl")) {
		ret = md_platform_on(md, true);
	} else if (sysfs_streq(buf, "takeover")) {
		if (md->power.state == CCCI_MD_INHERITED)
			ret = ccci_dpmaif_quiesce(md->dpmaif);
		else
			ret = 0;
		if (!ret)
			ret = ccci_md_power_takeover(&md->power);
	} else if (sysfs_streq(buf, "off")) {
		ret = md_platform_off(md);
	} else {
		ret = -EINVAL;
	}
	if (ret)
		dev_err(dev, "MD platform request failed at %s: %d\n", md->power.step, ret);
	mutex_unlock(&md->lock);
	return ret ?: count;
}
static DEVICE_ATTR_RW(md_power);

static ssize_t md_boot_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ccci_md_platform *md = dev_get_drvdata(dev);
	ssize_t ret;

	mutex_lock(&md->lock);
	ret = ccci_md_boot_status(md->boot, buf);
	mutex_unlock(&md->lock);
	return ret;
}

static ssize_t md_boot_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct ccci_md_platform *md = dev_get_drvdata(dev);
	enum ccci_runtime_crystal crystal;
	enum ccci_rtc32k_source source;
	u64 generation;
	int ret;

	if (!md->boot)
		return -EOPNOTSUPP;
	mutex_lock(&md->lock);
	if (sscanf(buf, "prepare %llu", &generation) == 1) {
		if (md->power.state != CCCI_MD_POWERED || !md->power.clocks ||
		    !md->power.domain) {
			ret = -EHOSTDOWN;
			goto out;
		}
		ret = ccci_rtc32k_read(dev, &crystal, &source);
		if (!ret)
			dev_info(dev, "MD boot prepare: generation=%llu crystal=%u source=%u\n",
				 generation, crystal, source);
		if (!ret)
			ret = ccci_md_boot_prepare(md->boot, &md->smem, generation, crystal);
	} else if (sysfs_streq(buf, "rearm")) {
		if (md->power.state != CCCI_MD_STOPPED || md->power.clocks ||
		    md->power.domain || md->power.error || !md->dpmaif_off_reset) {
			ret = -EBUSY;
			goto out;
		}
		ret = md_domain_check(md, false);
		if (!ret)
			ret = ccci_md_boot_rearm(md->boot);
		if (!ret) {
			md->power.attempted = false;
			md->restore_rails = true;
		}
	} else if (sysfs_streq(buf, "release")) {
		ret = ccci_md_boot_release(md->boot, NULL);
	} else if (sysfs_streq(buf, "release-with-dl")) {
		if (!md->dpmaif) {
			ret = -EOPNOTSUPP;
			goto out;
		}
		if (md->power.state != CCCI_MD_POWERED || !md->power.clocks ||
		    !md->power.domain) {
			ret = -EHOSTDOWN;
			goto out;
		}
		ret = md_domain_check(md, true);
		if (!ret)
			ret = ccci_md_boot_release(md->boot, md->dpmaif);
	} else if (sysfs_streq(buf, "stop")) {
		/* Parent must have independently read MD1 OFF. */
		ret = ccci_md_boot_stop(md->boot, md->power.state == CCCI_MD_STOPPED);
	} else {
		ret = -EINVAL;
	}
out:
	mutex_unlock(&md->lock);
	return ret ?: count;
}
static DEVICE_ATTR_RW(md_boot);

static ssize_t ccif_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ccci_md_platform *md = dev_get_drvdata(dev);
	ssize_t ret;

	if (!md->ccif)
		return -EOPNOTSUPP;
	mutex_lock(&md->lock);
	ret = ccci_ccif_status(md->ccif, buf);
	mutex_unlock(&md->lock);
	return ret;
}

static ssize_t ccif_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct ccci_md_platform *md = dev_get_drvdata(dev);
	int ret;

	if (!md->ccif)
		return -EOPNOTSUPP;
	if (!sysfs_streq(buf, "prepare"))
		return -EINVAL;
	mutex_lock(&md->lock);
	if (md->power.state != CCCI_MD_POWERED || !md->power.clocks || !md->power.domain) {
		ret = -EHOSTDOWN;
	} else {
		/* The boot owner owns the rings once its one preparation starts. */
		ret = ccci_md_boot_offline(md->boot);
		if (!ret)
			ret = ccci_port_dispatch_offline(md->dispatch);
		if (!ret)
			ret = ccci_service_endpoint_offline(md->services);
		if (!ret)
			ret = ccci_ccism_bridge_offline(md->bridge);
		if (!ret)
			ret = md_domain_check(md, true);
		if (!ret)
			ret = ccci_ccif_prepare(md->ccif);
	}
	mutex_unlock(&md->lock);
	return ret ?: count;
}
static DEVICE_ATTR_RW(ccif);

static ssize_t ccif_fs_tx_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ccci_md_platform *md = dev_get_drvdata(dev);
	ssize_t ret = -ENODATA;

	if (!md->ccif)
		return -EOPNOTSUPP;
	mutex_lock(&md->lock);
	if (md->power.state == CCCI_MD_POWERED && md->power.domain && md->power.clocks)
		ret = ccci_ccif_fs_tx_status(md->ccif, buf);
	mutex_unlock(&md->lock);
	return ret;
}
static DEVICE_ATTR_RO(ccif_fs_tx);

static ssize_t handshake_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ccci_md_platform *md = dev_get_drvdata(dev);
	ssize_t ret;

	if (!md->ccif)
		return -EOPNOTSUPP;
	mutex_lock(&md->lock);
	ret = ccci_ccif_handshake_status(md->ccif, buf);
	mutex_unlock(&md->lock);
	return ret;
}
static DEVICE_ATTR_RO(handshake);

static ssize_t ccism_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ccci_md_platform *md = dev_get_drvdata(dev);

	if (!md->bridge)
		return -EOPNOTSUPP;
	return ccci_ccism_bridge_status(md->bridge, buf);
}
static DEVICE_ATTR_RO(ccism);

static ssize_t services_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ccci_md_platform *md = dev_get_drvdata(dev);
	ssize_t ret;

	mutex_lock(&md->lock);
	ret = ccci_service_endpoint_status(md->services, buf);
	mutex_unlock(&md->lock);
	return ret;
}
static DEVICE_ATTR_RO(services);

static ssize_t dispatch_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ccci_md_platform *md = dev_get_drvdata(dev);
	ssize_t ret;

	mutex_lock(&md->lock);
	ret = ccci_port_dispatch_status(md->dispatch, buf);
	mutex_unlock(&md->lock);
	return ret;
}
static DEVICE_ATTR_RO(dispatch);

/* Bounded decode of the v5 exception overview header, never the payload. */
#define MD_EE_RAW_MAX	(8 + CCCI_MD_EE_MAX_CORES * 12 + 0x11c)

static ssize_t md_ee_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ccci_md_platform *md = dev_get_drvdata(dev);
	struct ccci_md_ee ee;
	u8 raw[MD_EE_RAW_MAX];
	size_t bytes;
	int ret;

	mutex_lock(&md->lock);
	if (!md->ee || md->ee_bytes < 8) {
		ret = sysfs_emit(buf, "present=%u\n", md->ee ? 1 : 0);
		goto out;
	}
	bytes = min_t(size_t, md->ee_bytes, sizeof(raw));
	for (size_t i = 0; i < bytes; i++)
		raw[i] = readb(md->ee + i);
	ret = ccci_md_ee_decode(raw, bytes, &ee);
	if (ret) {
		ret = sysfs_emit(buf, "present=1 bytes=%zu error=%d\n", md->ee_bytes, ret);
		goto out;
	}
	/* core count is a record field, so every later offset derives from it. */
	if (ee.content_type == 1)
		ret = sysfs_emit(buf,
				 "present=1 bytes=%zu version=%#x cores=%u offender=%s ex_type=%u class=fatal section=%u cadefa=%u code=%#x,%#x,%#x sp=%#x pc=%#x lr=%#x addr=%#x cause=%#x status=%#x file_supported=%u file=%s\n",
				 md->ee_bytes, ee.version, ee.cores, ee.core, ee.ex_type,
				 ee.section, ee.cadefa, ee.code[0], ee.code[1], ee.code[2],
				 ee.sp, ee.pc, ee.lr, ee.addr, ee.cause, ee.status,
				 ee.filename_supported, ee.file);
	else
		ret = sysfs_emit(buf,
				 "present=1 bytes=%zu version=%#x cores=%u offender=%s ex_type=%u class=assert line=%u path=%s para=%#x,%#x,%#x lr=%#x\n",
				 md->ee_bytes, ee.version, ee.cores, ee.core, ee.ex_type,
				 ee.line, ee.path, ee.para[0], ee.para[1], ee.para[2], ee.lr);
out:
	mutex_unlock(&md->lock);
	return ret;
}
static DEVICE_ATTR_RO(md_ee);

/*
 * Read-only LK geometry: the MD image window (hdr_tbl_inf) and the shared
 * regions this driver already validated. Names and numbers only.
 */
static ssize_t lk_regions_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ccci_md_platform *md = dev_get_drvdata(dev);
	const struct ccci_smem_layout *smem = &md->smem;
	ssize_t ret;

	mutex_lock(&md->lock);
	ret = sysfs_emit(buf,
			 "bank0=0x%llx+0x%llx noncached=0x%llx+0x%llx cached=0x%llx+0x%llx users=%u cache_md_base=0x%x cache_gaps=%u noncache_gaps=%u\n",
			 smem->bank0.base, smem->bank0.size,
			 smem->noncached.base, smem->noncached.size,
			 smem->cached.base, smem->cached.size,
			 smem->count, smem->cache_md_base, smem->cache_gap_bytes,
			 smem->noncache_gap_bytes);
	mutex_unlock(&md->lock);
	return ret;
}
static DEVICE_ATTR_RO(lk_regions);

/*
 * The same validated table, one line per LK user, so it can be compared with
 * the vendor's own region log. Numbers only; no payload is reachable here.
 */
static ssize_t smem_users_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ccci_md_platform *md = dev_get_drvdata(dev);
	const struct ccci_smem_layout *smem = &md->smem;
	ssize_t ret = 0;

	mutex_lock(&md->lock);
	for (u32 i = 0; i < CCCI_SMEM_USERS && ret < PAGE_SIZE - 96; i++) {
		const struct ccci_smem_region *region = &smem->users[i];

		if (!region->present)
			continue;
		ret += sysfs_emit_at(buf, ret,
				     "id=%u ap=0x%llx md=0x%x size=0x%x cached=%u preserve=%u nomap=%u\n",
				     i, region->ap, region->md, region->size,
				     region->cached, region->preserve_first, region->no_map);
	}
	mutex_unlock(&md->lock);
	return ret;
}
static DEVICE_ATTR_RO(smem_users);

/*
 * Header-only view of the NVRAM cache region: enough to tell whether the vendor
 * stack left a populated cache behind. Bounded to 64 bytes and read-only.
 */
#define MD_CACHE_HEAD	64

static ssize_t smem_cache_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ccci_md_platform *md = dev_get_drvdata(dev);
	const struct ccci_smem_region *region = &md->smem.users[CCCI_SMEM_NVRAM];
	ssize_t ret = 0;
	void __iomem *map;
	u64 page;
	u32 bytes;

	mutex_lock(&md->lock);
	if (!region->present || region->size < MD_CACHE_HEAD) {
		ret = sysfs_emit(buf, "present=%u\n", region->present ? 1 : 0);
		goto out;
	}
	page = region->ap & ~0xfffULL;
	map = ioremap(page, region->ap - page + MD_CACHE_HEAD);
	if (!map) {
		ret = -ENOMEM;
		goto out;
	}
	ret = sysfs_emit(buf, "ap=0x%llx md=0x%x size=0x%x head=", region->ap,
			 region->md, region->size);
	for (bytes = 0; bytes < MD_CACHE_HEAD && ret < PAGE_SIZE - 8; bytes++)
		ret += sysfs_emit_at(buf, ret, "%02x", readb(map + region->ap - page + bytes));
	ret += sysfs_emit_at(buf, ret, "\n");
	iounmap(map);
out:
	mutex_unlock(&md->lock);
	return ret;
}
static DEVICE_ATTR_RO(smem_cache);

/* Explicit operator policy for the first-start clear; default keeps clearing. */
static ssize_t smem_clear_store(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t count)
{
	if (sysfs_streq(buf, "clear"))
		ccci_smem_clear_policy(true);
	else if (sysfs_streq(buf, "skip"))
		ccci_smem_clear_policy(false);
	else
		return -EINVAL;
	return count;
}

static ssize_t smem_clear_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n",
			  ccci_smem_clear_policy_enabled() ? "clear" : "skip");
}
static DEVICE_ATTR_RW(smem_clear);

/*
 * Operator bisection of the runtime feature set: bit N suppresses offer N
 * before the single publication, so one image can compare subsets. Refused
 * once the MD boot attempt has started.
 */
static ssize_t runtime_mask_store(struct device *dev, struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct ccci_md_platform *md = dev_get_drvdata(dev);
	u64 mask;
	int ret;

	ret = kstrtoull(buf, 16, &mask);
	if (ret)
		return ret;
	mutex_lock(&md->lock);
	ret = ccci_md_boot_set_feature_mask(md->boot, mask);
	mutex_unlock(&md->lock);
	return ret ? ret : count;
}

static ssize_t runtime_mask_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ccci_md_platform *md = dev_get_drvdata(dev);
	u64 mask;

	mutex_lock(&md->lock);
	mask = ccci_md_boot_feature_mask(md->boot);
	mutex_unlock(&md->lock);
	return sysfs_emit(buf, "0x%llx\n", mask);
}
static DEVICE_ATTR_RW(runtime_mask);

/* AT/MUX channel counters, with an explicit opt-in for the text preview. */
static ssize_t md_at_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ccci_md_platform *md = dev_get_drvdata(dev);
	ssize_t ret, used;

	ret = ccci_port_dispatch_at(md->dispatch, buf);
	if (ret < 0)
		return ret;
	used = ret;
	/* sysfs_emit() requires a page-aligned buffer, so append with a size. */
	ret = ccci_md_at_status(md->at, buf + used, PAGE_SIZE - used);
	if (ret < 0)
		return ret;
	used += ret;
	ret = ccci_md_net_status(md->net, buf + used, PAGE_SIZE - used);
	if (ret < 0)
		return ret;
	return used + ret;
}

static ssize_t md_at_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct ccci_md_platform *md = dev_get_drvdata(dev);
	bool enable;
	int ret;

	if (sysfs_streq(buf, "on"))
		enable = true;
	else if (sysfs_streq(buf, "off"))
		enable = false;
	else
		return -EINVAL;
	ret = ccci_port_dispatch_at_preview(md->dispatch, enable);
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(md_at);

/* Operator gate for the CCMNI data path; default off keeps the MD silent. */
static ssize_t md_net_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ccci_md_platform *md = dev_get_drvdata(dev);

	return ccci_md_net_status(md->net, buf, PAGE_SIZE);
}

static ssize_t md_net_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct ccci_md_platform *md = dev_get_drvdata(dev);
	int ret;

	if (sysfs_streq(buf, "on"))
		ret = ccci_md_net_enable(md->net, true);
	else if (sysfs_streq(buf, "off"))
		ret = ccci_md_net_enable(md->net, false);
	else
		return -EINVAL;
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(md_net);

/*
 * Printable text of the MD debug region, newest tail first. Binary record
 * fields are dropped rather than hex-dumped so the log stays readable; the
 * caller asked for this content explicitly and it never leaves the machine.
 */
#define MD_DBG_TEXT_MAX	3200

static ssize_t md_dbg_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ccci_md_platform *md = dev_get_drvdata(dev);
	size_t len = 0, start = 0;
	char *text;
	ssize_t ret;

	if (!md->dbg || !md->dbg_bytes)
		return -EOPNOTSUPP;
	text = kvmalloc(md->dbg_bytes + 1, GFP_KERNEL);
	if (!text)
		return -ENOMEM;
	mutex_lock(&md->lock);
	for (size_t i = 0; i < md->dbg_bytes; i++) {
		u8 c = readb(md->dbg + i);

		if (c >= 0x20 && c < 0x7f)
			text[len++] = c;
		else if (len && text[len - 1] != '\n')
			text[len++] = '\n';
	}
	mutex_unlock(&md->lock);
	while (len && text[len - 1] == '\n')
		len--;
	if (len > MD_DBG_TEXT_MAX) {
		const char *nl;

		start = len - MD_DBG_TEXT_MAX;
		nl = memchr(text + start, '\n', len - start);
		start = nl ? nl - text + 1 : start;
	}
	ret = sysfs_emit(buf, "bytes=%zu shown=%zu\n%.*s\n", md->dbg_bytes,
			 len - start, (int)(len - start), text + start);
	kvfree(text);
	return ret;
}
static DEVICE_ATTR_RO(md_dbg);

#define MD_DFD_CHUNK	2048

/*
 * Convert one window of the MD DFD region to text: printable bytes verbatim,
 * every other run collapsed to a single newline. State crosses windows so a
 * chunk boundary never invents or loses a separator.
 */
static size_t md_dfd_span(const void __iomem *dfd, loff_t *pos, loff_t end,
			  char *buf, size_t capacity, bool *nl)
{
	size_t n = 0;

	while (n < capacity && *pos < end) {
		u8 c = readb(dfd + *pos);

		(*pos)++;
		if (c >= 0x20 && c < 0x7f) {
			buf[n++] = c;
			*nl = false;
		} else if (!*nl) {
			buf[n++] = '\n';
			*nl = true;
		}
	}
	return n;
}

/*
 * Streaming text view of the MD DFD region. The MD owns this memory, so the
 * reader keeps no snapshot; it pages by file position.
 */
static ssize_t md_dfd_read(struct file *file, char __user *to, size_t count, loff_t *ppos)
{
	struct ccci_md_platform *md = file->private_data;
	loff_t pos = *ppos;
	size_t n;
	bool nl;
	char *buf;
	int ret;

	if (!md->dfd || !md->dfd_bytes)
		return -EOPNOTSUPP;
	if (pos < 0 || pos > md->dfd_bytes)
		return -EINVAL;
	if (pos == md->dfd_bytes)
		return 0;
	if (count > MD_DFD_CHUNK)
		count = MD_DFD_CHUNK;
	buf = kmalloc(count, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	mutex_lock(&md->lock);
	if (pos) {
		u8 prev = readb(md->dfd + pos - 1);

		nl = prev < 0x20 || prev >= 0x7f;
	} else {
		nl = true;
	}
	n = 0;
	/* A window of pure padding yields no text but still advances. */
	while (!n && pos < md->dfd_bytes)
		n = md_dfd_span(md->dfd, &pos, md->dfd_bytes, buf, count, &nl);
	mutex_unlock(&md->lock);
	ret = n;
	if (n && copy_to_user(to, buf, n))
		ret = -EFAULT;
	else
		*ppos = pos;
	kfree(buf);
	return ret;
}

static const struct file_operations md_dfd_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = md_dfd_read,
	.llseek = default_llseek,
};

static void md_dfd_debugfs_remove(void *data)
{
	debugfs_remove(data);
}

/* Whole-region scan: tells "no log yet" apart from "log with no text". */
static ssize_t md_dfd_info_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ccci_md_platform *md = dev_get_drvdata(dev);
	size_t nonzero = 0, printable = 0, first = md->dfd_bytes;
	ssize_t ret;

	if (!md->dfd || !md->dfd_bytes)
		return -EOPNOTSUPP;
	mutex_lock(&md->lock);
	for (size_t i = 0; i < md->dfd_bytes; i++) {
		u8 c = readb(md->dfd + i);

		if (c) {
			nonzero++;
			if (first == md->dfd_bytes)
				first = i;
			if (c >= 0x20 && c < 0x7f)
				printable++;
		}
	}
	mutex_unlock(&md->lock);
	ret = sysfs_emit(buf, "bytes=%zu nonzero=%zu printable=%zu first=%zd\n",
			 md->dfd_bytes, nonzero, printable,
			 first == md->dfd_bytes ? -1 : (ssize_t)first);
	return ret;
}
static DEVICE_ATTR_RO(md_dfd_info);

/* Raw header bytes for offline decode; no payload or core dump is exposed. */
static ssize_t md_ee_raw_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ccci_md_platform *md = dev_get_drvdata(dev);
	ssize_t ret;
	u32 bytes;

	mutex_lock(&md->lock);
	bytes = md->ee ? min_t(u32, (u32)md->ee_bytes, 0x1c0) : 0;
	for (u32 i = 0; i < bytes; i++)
		hex_byte_pack(buf + i * 2, readb(md->ee + i));
	ret = bytes * 2;
	buf[ret++] = '\n';
	buf[ret] = 0;
	mutex_unlock(&md->lock);
	return ret;
}
static DEVICE_ATTR_RO(md_ee_raw);

static struct attribute *md_attrs[] = {
	&dev_attr_md_power.attr, &dev_attr_md_boot.attr, &dev_attr_ccif.attr,
	&dev_attr_ccif_fs_tx.attr,
	&dev_attr_handshake.attr, &dev_attr_ccism.attr, &dev_attr_services.attr,
	&dev_attr_dispatch.attr, &dev_attr_md_ee.attr, &dev_attr_md_ee_raw.attr,
	&dev_attr_lk_regions.attr, &dev_attr_smem_users.attr,
	&dev_attr_smem_cache.attr, &dev_attr_smem_clear.attr,
	&dev_attr_runtime_mask.attr, &dev_attr_md_at.attr, &dev_attr_md_net.attr,
	&dev_attr_md_dbg.attr, &dev_attr_md_dfd_info.attr, NULL,
};

static const struct attribute_group md_group = { .attrs = md_attrs };

static void md_lookup_release(struct device *dev)
{
	of_node_put(dev->of_node);
	kfree(dev);
}

static int md_attach_domain(struct device *dev, struct ccci_md_platform *md)
{
	struct device_node *node;
	struct device *lookup;
	int ret;

	/* A domain on the platform node would be powered before probe runs. */
	if (of_property_present(dev->of_node, "power-domains"))
		return -EINVAL;
	node = of_get_child_by_name(dev->of_node, "power");
	if (!node)
		return -EINVAL;
	if (!of_device_is_available(node) ||
	    of_count_phandle_with_args(node, "power-domains", "#power-domain-cells") != 1) {
		of_node_put(node);
		return -EINVAL;
	}
	lookup = kzalloc_obj(*lookup);
	if (!lookup) {
		of_node_put(node);
		return -ENOMEM;
	}
	lookup->parent = dev;
	lookup->of_node = node;
	lookup->release = md_lookup_release;
	device_initialize(lookup);
	ret = dev_set_name(lookup, "%s-power", dev_name(dev));
	if (ret)
		goto put;
	ret = device_add(lookup);
	if (ret)
		goto put;
	/* attach_by_id creates a runtime-suspended device WITHOUT powering on. */
	md->domain = dev_pm_domain_attach_by_id(lookup, 0);
	if (IS_ERR_OR_NULL(md->domain)) {
		ret = md->domain ? PTR_ERR(md->domain) : -ENODEV;
		device_unregister(lookup);
		return ret;
	}
	md->lookup = lookup;
	return 0;
put:
	put_device(lookup);
	return ret;
}

int ccci_md_platform_create(struct platform_device *pdev, const struct ccci_smem_layout *layout)
{
	static const char * const supplies[] = { "md-vmodem", "md-vnr", "md-vsram", "md-vdigrf" };
	static const char * const clocks[] = { "ccif-ap", "ccif-md", "ccif1-ap", "ccif1-md" };
	struct device *dev = &pdev->dev;
	struct ccci_md_platform *md;
	int ret;

	md = devm_kzalloc(dev, sizeof(*md), GFP_KERNEL);
	if (!md)
		return -ENOMEM;
	mutex_init(&md->lock);
	md->smem = *layout;
	md->spm = syscon_regmap_lookup_by_phandle(dev->of_node, "mediatek,spm");
	if (IS_ERR(md->spm))
		return dev_err_probe(dev, PTR_ERR(md->spm), "SPM regmap unavailable\n");
	md->infra = syscon_regmap_lookup_by_phandle(dev->of_node, "mediatek,infracfg");
	if (IS_ERR(md->infra))
		return dev_err_probe(dev, PTR_ERR(md->infra), "infracfg regmap unavailable\n");
	for (unsigned int i = 0; i < ARRAY_SIZE(md->rails); i++) {
		md->rails[i] = devm_regulator_get_optional(dev, supplies[i]);
		if (IS_ERR(md->rails[i]))
			return dev_err_probe(dev, PTR_ERR(md->rails[i]),
					     "%s unavailable\n", supplies[i]);
		md->clocks[i].id = clocks[i];
	}
	ret = devm_clk_bulk_get(dev, ARRAY_SIZE(md->clocks), md->clocks);
	if (ret)
		return dev_err_probe(dev, ret, "ordinary CCIF clocks unavailable\n");
	ret = ccci_md_power_init(&md->power, &md_power_ops, md);
	if (ret)
		return ret;
	ret = md_off_check(md);
	if (ret == -EBUSY) {
		ret = md_domain_check(md, true);
		if (!ret) {
			md->power.state = CCCI_MD_INHERITED;
			md->power.step = "lk-on-held";
		}
	}
	if (ret)
		return dev_err_probe(dev, ret, "MD initial state is incomplete\n");
	ret = md_read_rails(md, md->voltage);
	if (!ret)
		ret = md_rails_check(md);
	if (ret)
		return dev_err_probe(dev, ret, "cold rail baseline unavailable/unstable\n");
	md->dpmaif = ccci_dpmaif_find(dev);
	if (IS_ERR(md->dpmaif))
		return dev_err_probe(dev, PTR_ERR(md->dpmaif), "DPMAIF unavailable\n");
	ret = ccci_dpmaif_quiesce(md->dpmaif);
	if (ret)
		return dev_err_probe(dev, ret, "DPMAIF not quiescent\n");
	if (md->smem.users[CCCI_SMEM_MDSS_DBG].present) {
		const struct ccci_smem_region *ee = &md->smem.users[CCCI_SMEM_MDSS_DBG];
		u32 bytes = ee->size;
		u64 page = ee->ap & ~0xfffULL;
		u64 end = (ee->ap + bytes + 0xfff) & ~0xfffULL;
		bool inside = ccci_lk_contains(&md->smem.noncached, page, end - page) ||
			      ccci_lk_contains(&md->smem.cached, page, end - page);

		if (inside && devm_request_mem_region(dev, page, end - page, "ccci-md-ee")) {
			void __iomem *map = devm_ioremap(dev, page, end - page);

			if (map) {
				md->ee = map + (ee->ap - page);
				md->ee_bytes = bytes;
				/* Same reservation: the MD also writes its log here. */
				md->dbg = md->ee;
				md->dbg_bytes = bytes;
			}
		}
	}
	if (md->smem.users[CCCI_SMEM_DFD].present) {
		const struct ccci_smem_region *dfd = &md->smem.users[CCCI_SMEM_DFD];
		u32 bytes = dfd->size;
		u64 page = dfd->ap & ~0xfffULL;
		u64 end = (dfd->ap + bytes + 0xfff) & ~0xfffULL;
		bool inside = ccci_lk_contains(&md->smem.noncached, page, end - page) ||
			      ccci_lk_contains(&md->smem.cached, page, end - page);

		if (bytes && inside &&
		    devm_request_mem_region(dev, page, end - page, "ccci-md-dfd")) {
			void __iomem *map = devm_ioremap(dev, page, end - page);

			if (map) {
				md->dfd = map + (dfd->ap - page);
				md->dfd_bytes = bytes;
			}
		}
	}
	if (IS_ENABLED(CONFIG_OP6893_CCCI_CCIF)) {
		md->ccif = ccci_ccif_create(pdev, layout, md->infra);
		if (IS_ERR(md->ccif))
			return dev_err_probe(dev, PTR_ERR(md->ccif), "CCIF reservation failed\n");
	}
	if (IS_ENABLED(CONFIG_OP6893_CCCI_CCISM)) {
		md->bridge = ccci_ccism_bridge_create(pdev, md->ccif, layout);
		if (IS_ERR(md->bridge))
			return dev_err_probe(dev, PTR_ERR(md->bridge),
					     "CCISM reservation failed\n");
	}
	ret = md_attach_domain(dev, md);
	if (ret)
		return dev_err_probe(dev, ret, "MD1 domain attachment failed\n");
	if (IS_ENABLED(CONFIG_OP6893_CCCI_SERVICES)) {
		md->services = ccci_service_endpoint_create(dev, md->ccif);
		if (IS_ERR(md->services)) {
			ret = PTR_ERR(md->services);
			dev_pm_domain_detach(md->domain, false);
			device_unregister(md->lookup);
			return dev_err_probe(dev, ret, "service endpoint unavailable\n");
		}
		if (of_machine_is_compatible("realme,op6893") ||
		    of_machine_is_compatible("infinix,x6710")) {
			const struct ccci_rpc_provider *provider =
				of_machine_is_compatible("infinix,x6710") ?
				ccci_rpc_x6710_provider() : ccci_rpc_op6893_provider();
			ret = ccci_service_endpoint_set_rpc(md->services, provider);
			if (ret) {
				dev_pm_domain_detach(md->domain, false);
				device_unregister(md->lookup);
				return dev_err_probe(dev, ret, "board RPC facts unavailable\n");
			}
		}
	}
	if (IS_ENABLED(CONFIG_OP6893_CCCI_DISPATCH)) {
		md->dispatch = ccci_port_dispatch_create(dev, md->ccif, md->bridge, md->services);
		if (IS_ERR(md->dispatch)) {
			ret = PTR_ERR(md->dispatch);
			dev_pm_domain_detach(md->domain, false);
			device_unregister(md->lookup);
			return dev_err_probe(dev, ret, "port dispatcher unavailable\n");
		}
		md->at = ccci_md_at_create(dev, md->ccif);
		if (IS_ERR(md->at)) {
			ret = PTR_ERR(md->at);
			dev_pm_domain_detach(md->domain, false);
			device_unregister(md->lookup);
			return dev_err_probe(dev, ret, "AT byte pipe unavailable\n");
		}
		ret = ccci_port_dispatch_set_at(md->dispatch, md->at);
		if (ret) {
			dev_pm_domain_detach(md->domain, false);
			device_unregister(md->lookup);
			return dev_err_probe(dev, ret, "AT pipe attachment refused\n");
		}
		md->net = ccci_md_net_create(dev, md->ccif);
		if (IS_ERR(md->net)) {
			ret = PTR_ERR(md->net);
			dev_pm_domain_detach(md->domain, false);
			device_unregister(md->lookup);
			return dev_err_probe(dev, ret, "CCMNI netdev unavailable\n");
		}
		ret = ccci_port_dispatch_set_net(md->dispatch, md->net);
		if (ret) {
			dev_pm_domain_detach(md->domain, false);
			device_unregister(md->lookup);
			return dev_err_probe(dev, ret, "CCMNI attachment refused\n");
		}
	}
	if (IS_ENABLED(CONFIG_OP6893_CCCI_NVRAM_CACHE)) {
		md->cache = ccci_nvram_cache_create(dev, &md->smem);
		if (IS_ERR(md->cache)) {
			ret = PTR_ERR(md->cache);
			dev_pm_domain_detach(md->domain, false);
			device_unregister(md->lookup);
			return dev_err_probe(dev, ret, "NVRAM cache geometry unavailable\n");
		}
	}
	if (IS_ENABLED(CONFIG_OP6893_CCCI_MD_BOOT)) {
		md->boot = ccci_md_boot_create(dev, md->ccif, md->dispatch, md->cache);
		if (IS_ERR(md->boot)) {
			ret = PTR_ERR(md->boot);
			dev_pm_domain_detach(md->domain, false);
			device_unregister(md->lookup);
			return dev_err_probe(dev, ret, "MD boot owner unavailable\n");
		}
	}
	platform_set_drvdata(pdev, md);
	ret = devm_device_add_group(dev, &md_group);
	if (ret) {
		platform_set_drvdata(pdev, NULL);
		dev_pm_domain_detach(md->domain, false);
		device_unregister(md->lookup);
		return ret;
	}
	if (md->dfd) {
		struct dentry *dentry;

		dentry = debugfs_create_file("op6893-md-dfd", 0400, NULL, md, &md_dfd_fops);
		if (!IS_ERR_OR_NULL(dentry)) {
			ret = devm_add_action_or_reset(dev, md_dfd_debugfs_remove, dentry);
			if (ret) {
				platform_set_drvdata(pdev, NULL);
				dev_pm_domain_detach(md->domain, false);
				device_unregister(md->lookup);
				return ret;
			}
		}
	}
	dev_info(dev, "MD owner ready: step=%s, SMEM retained, four rails/clocks and MD1 attached; CPU release not issued\n",
		 md->power.step);
	return 0;
}

void ccci_md_platform_shutdown(struct platform_device *pdev)
{
	struct ccci_md_platform *md = platform_get_drvdata(pdev);
	int ret;

	if (!md)
		return;
	mutex_lock(&md->lock);
	ret = md_platform_off(md);
	if (ret)
		dev_warn(&pdev->dev, "MD shutdown held at %s: %d\n", md->power.step, ret);
	mutex_unlock(&md->lock);
}
