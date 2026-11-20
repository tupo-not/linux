// SPDX-License-Identifier: GPL-2.0
/*
 * mt6893-mt6359.c -- Infinix Note 30 VIP (X6710, MT6893) speaker card.
 *
 * Playback leaves the AP through AFE I2S3 into a stereo pair of Awinic
 * AW87390 I2C smart PAs on i2c6 (0x58 left, 0x59 right), one per speaker.
 * The AW87390 driver (sound/soc/codecs/aw87390.c) registers a component with
 * no DAI - it is configured entirely over I2C from aw87390_acf.bin - so the
 * amps attach to the card as aux-devs and are driven purely by DAPM; the I2S3
 * back end uses a dummy codec and only supplies MCLK/BCLK/LRCK.
 *
 * The MT6359 PMIC codec handles the microphone path; it is not part of the
 * speaker playback link.  All the MTK AFE/DPCM plumbing below is shared with
 * the upstream mt8192 cards.
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/clk.h>
#include <linux/pm_runtime.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "../common/mtk-soc-card.h"
#include "../common/mtk-afe-platform-driver.h"
#include "../common/mtk-base-afe.h"
#include "../common/mtk-soundcard-driver.h"
#include "mt8192-afe-clk.h"
#include "mt8192-afe-common.h"
#include "mt8192-afe-gpio.h"

#define DRIVER_NAME		"mt6893_mt6359"
#define CARD_NAME		"mt6893_mt6359"

/* FE: AFE DL1 */
SND_SOC_DAILINK_DEFS(playback1,
		     DAILINK_COMP_ARRAY(COMP_CPU("DL1")),
		     DAILINK_COMP_ARRAY(COMP_DUMMY()),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()));

/* BE: AFE I2S3 -> dummy codec (the AW87390 PAs are aux-devs on this link) */
SND_SOC_DAILINK_DEFS(i2s3,
		     DAILINK_COMP_ARRAY(COMP_CPU("I2S3")),
		     DAILINK_COMP_ARRAY(COMP_DUMMY()),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()));

static struct device *mt6893_afe_dev;

static struct mtk_base_afe *mt6893_afe_from_rtd(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_component *cmpnt = snd_soc_rtdcom_lookup(rtd, AFE_PCM_NAME);

	return cmpnt ? snd_soc_component_get_drvdata(cmpnt) : NULL;
}

static int mt6893_i2s_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct mtk_base_afe *afe = mt6893_afe_from_rtd(rtd);

	if (afe)
		mt8192_mck_disable(afe, MT8192_I2S3_MCK);

	if (mt6893_afe_dev) {
		pm_runtime_put_autosuspend(mt6893_afe_dev);
		mt6893_afe_dev = NULL;
	}

	return 0;
}

static int mt6893_i2s_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	unsigned int rate = params_rate(params);
	unsigned int mclk_fs = rate * 128;
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct mtk_base_afe *afe;
	int ret;

	ret = snd_soc_dai_set_sysclk(cpu_dai, 0, mclk_fs, SND_SOC_CLOCK_OUT);
	if (ret)
		return ret;

	afe = mt6893_afe_from_rtd(rtd);
	if (afe) {
		mt6893_afe_dev = afe->dev;
		pm_runtime_resume_and_get(afe->dev);

		/* AUDIO_TOP_CON0 bit2 (aud_afe pd) and CON1 bit6 (I2S3 gate),
		 * both active low - the clock framework does not enable these
		 * on this board (shared audsys regmap still cache-only). */
		regmap_clear_bits(afe->regmap, 0x0, BIT(2));
		regmap_clear_bits(afe->regmap, 0x4, BIT(6));

		ret = mt8192_mck_enable(afe, MT8192_I2S3_MCK, mclk_fs);
		if (ret)
			dev_warn(rtd->card->dev,
				 "failed to enable I2S3 mclk: %d\n", ret);
	}

	return 0;
}

static const struct snd_soc_ops mt6893_i2s_ops = {
	.hw_params = mt6893_i2s_hw_params,
	.hw_free = mt6893_i2s_hw_free,
};

/* Pin the back end to 32-bit samples (the stock vendor tune uses S32_LE). */
static int mt6893_i2s_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
				      struct snd_pcm_hw_params *params)
{
	snd_mask_reset_range(hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT),
			     0, (__force unsigned int)SNDRV_PCM_FORMAT_LAST);
	params_set_format(params, SNDRV_PCM_FORMAT_S32_LE);

	return 0;
}

static struct snd_soc_dai_link mt6893_mt6359_dai_links[] = {
	{
		.name = "Playback_1",
		.stream_name = "Playback_1",
		.trigger = { SND_SOC_DPCM_TRIGGER_PRE,
			     SND_SOC_DPCM_TRIGGER_PRE },
		.dynamic = 1,
		.playback_only = 1,
		.ops = &mtk_soundcard_common_playback_ops,
		SND_SOC_DAILINK_REG(playback1),
	},
	{
		.name = "I2S3",
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_CBC_CFC,
		.ops = &mt6893_i2s_ops,
		.be_hw_params_fixup = mt6893_i2s_hw_params_fixup,
		.no_pcm = 1,
		.playback_only = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(i2s3),
	},
};

/*
 * The AFE connection matrix is a DAPM mixer: playback reaches I2S3 only when
 * the "DL1 -> I2S3_CHx" switches are on.  No UCM here, so flip them once the
 * card widgets exist.
 */
static const char * const mt6893_afe_matrix_switches[] = {
	"I2S3_CH1 DL1_CH1",
	"I2S3_CH2 DL1_CH2",
};

static void mt6893_afe_route_work(struct work_struct *work);
static DECLARE_DELAYED_WORK(mt6893_afe_route_dw, mt6893_afe_route_work);
static struct snd_soc_card *mt6893_afe_route_card;

static void mt6893_afe_route_work(struct work_struct *work)
{
	struct snd_ctl_elem_value ucontrol = {};
	struct snd_kcontrol *kctl;
	int i, ret;

	if (!mt6893_afe_route_card)
		return;

	for (i = 0; i < ARRAY_SIZE(mt6893_afe_matrix_switches); i++) {
		const char *name = mt6893_afe_matrix_switches[i];

		kctl = snd_soc_card_get_kcontrol(mt6893_afe_route_card, name);
		if (!kctl) {
			dev_warn(mt6893_afe_route_card->dev,
				 "AFE route %s not found\n", name);
			continue;
		}

		memset(&ucontrol, 0, sizeof(ucontrol));
		ucontrol.value.integer.value[0] = 1;
		ret = kctl->put(kctl, &ucontrol);
		if (ret < 0)
			dev_err(mt6893_afe_route_card->dev,
				"could not enable AFE route %s: %d\n",
				name, ret);
	}
}

/* DAPM: drive the two AW87390 amps from the I2S3 output.  The amps' input
 * widgets are named "IN" (see aw87390.c). */
static const struct snd_soc_dapm_widget mt6893_dapm_widgets[] = {
	SND_SOC_DAPM_SPK("Speakers", NULL),
};

static const struct snd_soc_dapm_route mt6893_dapm_routes[] = {
	{ "IN", NULL, "I2S3" },
	{ "Speakers", NULL, "OUT" },
};

static int mt6893_mt6359_late_probe(struct snd_soc_card *card)
{
	struct snd_ctl_elem_value ucontrol = {};
	struct snd_kcontrol *kctl;

	mt6893_afe_route_card = card;
	schedule_delayed_work(&mt6893_afe_route_dw, msecs_to_jiffies(1000));

	/* I2S3_HD_Mux -> Low_Jitter (HD mode, 32-bit slot format). */
	kctl = snd_soc_card_get_kcontrol(card, "I2S3_HD_Mux");
	if (kctl) {
		memset(&ucontrol, 0, sizeof(ucontrol));
		ucontrol.value.enumerated.item[0] = 1;
		kctl->put(kctl, &ucontrol);
	}

	return 0;
}

static struct snd_soc_card mt6893_mt6359_card = {
	.name = CARD_NAME,
	.owner = THIS_MODULE,
	.dai_link = mt6893_mt6359_dai_links,
	.num_links = ARRAY_SIZE(mt6893_mt6359_dai_links),
	.dapm_widgets = mt6893_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(mt6893_dapm_widgets),
	.dapm_routes = mt6893_dapm_routes,
	.num_dapm_routes = ARRAY_SIZE(mt6893_dapm_routes),
	.late_probe = mt6893_mt6359_late_probe,
};

static int mt6893_mt6359_soc_card_probe(struct mtk_soc_card_data *soc_card_data,
					bool legacy)
{
	struct snd_soc_card *card = soc_card_data->card_data->card;

	/* Attach the AW87390 PAs listed in the sound node's "aux-devs". */
	return snd_soc_of_parse_aux_devs(card, "aux-devs");
}

static const unsigned int mt6893_pcm_playback_channels[] = { 1, 2 };
static const unsigned int mt6893_pcm_playback_rates[] = { 44100, 48000 };

static const struct mtk_pcm_constraints_data mt6893_pcm_constraints[] = {
	[MTK_CONSTRAINT_PLAYBACK] = {
		.channels = &(const struct snd_pcm_hw_constraint_list) {
			.list = mt6893_pcm_playback_channels,
			.count = ARRAY_SIZE(mt6893_pcm_playback_channels),
		},
		.rates = &(const struct snd_pcm_hw_constraint_list) {
			.list = mt6893_pcm_playback_rates,
			.count = ARRAY_SIZE(mt6893_pcm_playback_rates),
		},
	},
};

static const struct mtk_soundcard_pdata mt6893_mt6359_pdata = {
	.card_name = CARD_NAME,
	.card_data = &(struct mtk_platform_card_data) {
		.card = &mt6893_mt6359_card,
		.num_jacks = 0,
		.pcm_constraints = mt6893_pcm_constraints,
		.num_pcm_constraints = ARRAY_SIZE(mt6893_pcm_constraints),
	},
	.soc_probe = mt6893_mt6359_soc_card_probe,
};

static const struct of_device_id mt6893_mt6359_dt_match[] = {
	{ .compatible = "mediatek,mt6893-mt6359", .data = &mt6893_mt6359_pdata },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mt6893_mt6359_dt_match);

static struct platform_driver mt6893_mt6359_driver = {
	.probe = mtk_soundcard_common_probe,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = mt6893_mt6359_dt_match,
	},
};
module_platform_driver(mt6893_mt6359_driver);

MODULE_DESCRIPTION("MT6893-MT6359/AW87390 ALSA SoC machine driver");
MODULE_LICENSE("GPL v2");
