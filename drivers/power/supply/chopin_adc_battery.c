// SPDX-License-Identifier: GPL-2.0
/*
 * chopin: battery capacity from the charger's VBAT ADC.
 *
 * The board has a TI BQ27Z561 fuel gauge on i2c0 at 0x55, but its register
 * interface does not work with mainline's bq27xxx driver - every read returns
 * 0xFF and two-byte writes are NAKed, because the vendor TI driver performs an
 * init sequence (unseal/checksum/ROM mode) that mainline has no equivalent for.
 * So the gauge reports capacity 65535 and upower shows nothing useful.
 *
 * The MT6360 charger ADC does expose vbat (IIO channel 4, "vbat") and works, so
 * derive a capacity from the open-circuit voltage with a piecewise Li-ion curve.
 * It is an estimate, not coulomb counting, but it is monotonic enough to be
 * useful and it is what makes GNOME show a battery percentage at all.
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/iio/consumer.h>

#define CHOPIN_BAT_SAMPLES	8
#define CHOPIN_BAT_HYST		2	/* percent */

struct chopin_bat {
	struct device *dev;
	struct iio_channel *vbat;
	struct power_supply *psy;
	struct power_supply *chg;

	/* smoothing: the ADC reading moves with load and with the charger's
	 * CV taper, and toggling a status-dependent offset made the number jump
	 * between 14% and 23% while upower polled it. */
	int samples[CHOPIN_BAT_SAMPLES];
	int nsamples;
	int idx;
	int last_cap;
	bool cap_valid;
};

/* Open-circuit voltage -> capacity, derived from the stock
 * battery0_profile_t1 (25 C) table in the vendor DT: column 1 is OCV in
 * 0.1 mV units, column 0 is used charge out of 50855 (0.1 mAh).  Every third
 * row is kept plus the endpoints; the curve is monotonically descending.
 */
static const struct { int mv; int pct; } chopin_curve[] = {
	{ 4438, 100 }, { 4402,  97 }, { 4371,  94 }, { 4339,  91 },
	{ 4304,  88 }, { 4267,  85 }, { 4230,  82 }, { 4192,  79 },
	{ 4156,  76 }, { 4121,  73 }, { 4087,  70 }, { 4055,  67 },
	{ 4025,  64 }, { 3996,  61 }, { 3965,  58 }, { 3921,  55 },
	{ 3894,  52 }, { 3874,  48 }, { 3857,  45 }, { 3842,  42 },
	{ 3828,  39 }, { 3816,  36 }, { 3806,  33 }, { 3796,  30 },
	{ 3781,  27 }, { 3766,  24 }, { 3752,  21 }, { 3737,  18 },
	{ 3718,  15 }, { 3699,  12 }, { 3695,   9 }, { 3667,   6 },
	{ 3499,   3 }, { 2836,   0 },
};

static int chopin_vbat_to_capacity(int mv)
{
	int i;

	if (mv >= chopin_curve[0].mv)
		return 100;

	for (i = 1; i < ARRAY_SIZE(chopin_curve); i++) {
		if (mv >= chopin_curve[i].mv) {
			int hi = chopin_curve[i - 1].mv, hi_p = chopin_curve[i - 1].pct;
			int lo = chopin_curve[i].mv, lo_p = chopin_curve[i].pct;

			return lo_p + (mv - lo) * (hi_p - lo_p) / (hi - lo);
		}
	}

	return 0;
}

/* iio_read_channel_processed() returns microvolts for voltage channels. */
static int chopin_bat_read_vbat_uv(struct chopin_bat *bat, int *uv)
{
	return iio_read_channel_processed(bat->vbat, uv);
}

static int chopin_bat_charger_status(struct chopin_bat *bat)
{
	union power_supply_propval val = { .intval = POWER_SUPPLY_STATUS_UNKNOWN };

	if (!bat->chg)
		return POWER_SUPPLY_STATUS_UNKNOWN;

	if (power_supply_get_property(bat->chg, POWER_SUPPLY_PROP_STATUS, &val))
		return POWER_SUPPLY_STATUS_UNKNOWN;

	return val.intval;
}

static int chopin_bat_get_property(struct power_supply *psy,
				   enum power_supply_property prop,
				   union power_supply_propval *val)
{
	struct chopin_bat *bat = power_supply_get_drvdata(psy);
	int uv = 0;

	switch (prop) {
	case POWER_SUPPLY_PROP_CAPACITY: {
		int mv, cap, sum = 0, i;

		if (chopin_bat_read_vbat_uv(bat, &uv))
			return -EIO;

		mv = uv / 1000;

		/* moving average over the last few raw readings */
		bat->samples[bat->idx] = mv;
		bat->idx = (bat->idx + 1) % CHOPIN_BAT_SAMPLES;
		if (bat->nsamples < CHOPIN_BAT_SAMPLES)
			bat->nsamples++;

		for (i = 0; i < bat->nsamples; i++)
			sum += bat->samples[i];

		cap = chopin_vbat_to_capacity(sum / bat->nsamples);

		/* Only move the reported value when it changes by more than the
		 * hysteresis, so a flopping charger status cannot make the icon
		 * jump by 9%. */
		if (!bat->cap_valid) {
			bat->last_cap = cap;
			bat->cap_valid = true;
		} else if (abs(cap - bat->last_cap) >= CHOPIN_BAT_HYST) {
			bat->last_cap = cap;
		}

		val->intval = bat->last_cap;
		break;
	}
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		if (chopin_bat_read_vbat_uv(bat, &uv))
			return -EIO;
		val->intval = uv;
		break;
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = chopin_bat_charger_status(bat);
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static enum power_supply_property chopin_bat_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
};

static const struct power_supply_desc chopin_bat_desc = {
	.name		= "BAT0",
	.type		= POWER_SUPPLY_TYPE_BATTERY,
	.properties	= chopin_bat_props,
	.num_properties	= ARRAY_SIZE(chopin_bat_props),
	.get_property	= chopin_bat_get_property,
};

static int chopin_bat_probe(struct platform_device *pdev)
{
	struct power_supply_config cfg = {};
	struct chopin_bat *bat;
	struct device *dev = &pdev->dev;

	bat = devm_kzalloc(dev, sizeof(*bat), GFP_KERNEL);
	if (!bat)
		return -ENOMEM;

	bat->dev = dev;
	bat->vbat = devm_iio_channel_get(dev, "vbat");
	if (IS_ERR(bat->vbat))
		return dev_err_probe(dev, PTR_ERR(bat->vbat),
				     "no vbat channel\n");

	bat->chg = devm_power_supply_get_by_reference(dev, "power-supplies");
	if (IS_ERR(bat->chg))
		bat->chg = NULL;

	cfg.drv_data = bat;
	cfg.fwnode = dev_fwnode(dev);
	bat->psy = devm_power_supply_register(dev, &chopin_bat_desc, &cfg);
	if (IS_ERR(bat->psy))
		return dev_err_probe(dev, PTR_ERR(bat->psy),
				     "cannot register power supply\n");

	dev_info(dev, "battery capacity from MT6360 vbat ADC\n");
	return 0;
}

static const struct of_device_id chopin_bat_of_match[] = {
	{ .compatible = "chopin,adc-battery" },
	{}
};
MODULE_DEVICE_TABLE(of, chopin_bat_of_match);

static struct platform_driver chopin_bat_driver = {
	.driver = {
		.name = "chopin-adc-battery",
		.of_match_table = chopin_bat_of_match,
	},
	.probe = chopin_bat_probe,
};
module_platform_driver(chopin_bat_driver);

MODULE_DESCRIPTION("chopin battery capacity from the MT6360 VBAT ADC");
MODULE_LICENSE("GPL");
