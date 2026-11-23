# chopin: build the vendor glue the mediatek_v2 display stack links against.
# NOTE: the reference clone (port/mt6895-linux) was fetched with a sparse
# checkout, so drivers/misc/mediatek/{smi,mminfra,iommu,...} are not present yet;
# widen it (see port/V2-IMPORT.md) before wiring those in.
obj-$(CONFIG_MTK_CMDQ_MBOX_EXT) += cmdq/
obj-$(CONFIG_MTK_NVRAM_STAGE) += mtk_nvram_stage.o
obj-$(CONFIG_MTK_COMBO) += connectivity/
obj-$(CONFIG_MTK_BTIF) += btif/
obj-$(CONFIG_MTK_SCP_CHOPIN) += scp-chopin.o
obj-$(CONFIG_MTK_TINYSYS_SCP_SUPPORT) += scp/
obj-$(CONFIG_MTK_SENSORHUB) += sensor/2.0/
obj-y += ccci/
