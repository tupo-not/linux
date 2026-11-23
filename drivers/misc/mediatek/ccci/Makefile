# SPDX-License-Identifier: GPL-2.0-only
obj-$(CONFIG_OP6893_CCCI_LK_INSPECT) += op6893-ccci-lk.o
op6893-ccci-lk-y := ccci-lk.o ccci-lk-inspect.o
op6893-ccci-lk-$(CONFIG_OP6893_CCCI_SMEM) += ccci-smem.o
op6893-ccci-lk-$(CONFIG_OP6893_CCCI_MD_PLATFORM) += ccci-md-power.o ccci-md-platform.o ccci-md-ee.o
op6893-ccci-lk-$(CONFIG_OP6893_CCCI_CCIF) += ccci-ccif.o ccci-ccif-ring.o ccci-ccif-hw.o \
	ccci-ccif-protocol.o ccci-ccif-rx.o
op6893-ccci-lk-$(CONFIG_OP6893_CCCI_HANDSHAKE) += ccci-runtime.o ccci-runtime-profile.o ccci-ccif-tx.o \
	ccci-handshake.o ccci-ccif-control.o
obj-$(CONFIG_OP6893_CCCI_DPMAIF) += ccci-dpmaif.o ccci-dpmaif-format.o
obj-$(CONFIG_OP6893_CCCI_RESOURCES) += ccci-resources.o
obj-$(CONFIG_OP6893_CCCI_SCP_PROTOCOL) += ccci-scp-protocol.o
obj-$(CONFIG_OP6893_CCCI_SCP_PROTOCOL) += ccci-scp-memory.o
obj-$(CONFIG_OP6893_CCCI_SCP_RESTORE_PLAN) += ccci-scp-restore.o
obj-$(CONFIG_OP6893_CCCI_SCP_AWAKE) += ccci-scp-awake.o
obj-$(CONFIG_OP6893_CCCI_SCP_RESOURCE_HOLD) += ccci-scp-resource.o ccci-scp-resource-hold.o
obj-$(CONFIG_OP6893_CCCI_SCP_OBSERVE) += ccci-scp-observe.o
obj-$(CONFIG_OP6893_CCCI_SCP_RX) += ccci-scp-rx.o
obj-$(CONFIG_OP6893_CCCI_SCP_DRAIN) += ccci-scp-drain.o
obj-$(CONFIG_OP6893_CCCI_SCP_QUEUE) += ccci-scp-queue.o
obj-$(CONFIG_OP6893_CCCI_SCP_IRQ_RESERVE) += ccci-scp-irq.o
obj-$(CONFIG_OP6893_CCCI_SCP_MMIO) += ccci-scp-mmio.o
obj-$(CONFIG_OP6893_CCCI_SCP_RECEIVE) += ccci-scp-receive.o
obj-$(CONFIG_OP6893_CCCI_SCP_OWNER_OBSERVE) += ccci-scp-inspect.o
obj-$(CONFIG_OP6893_CCCI_SCP_RESET_ASSERT) += ccci-scp-reset.o
obj-$(CONFIG_OP6893_CCCI_SCP_RUNTIME) += ccci-scp-runtime.o ccci-scp-client.o
obj-$(CONFIG_OP6893_CCCI_CCISM) += ccci-ccism.o ccci-ccism-session.o ccci-ccism-bridge.o
op6893-ccci-lk-$(CONFIG_OP6893_CCCI_SERVICES) += ccci-fs-protocol.o ccci-rpc-protocol.o \
	ccci-service-router.o ccci-service-session.o ccci-service-endpoint.o ccci-rpc-provider.o ccci-rpc-op6893.o \
	ccci-rpc-x6710.o \
	ccci-rpc-lhif.o
op6893-ccci-lk-$(CONFIG_OP6893_CCCI_DISPATCH) += ccci-port-dispatch.o ccci-md-at.o ccci-md-net.o
op6893-ccci-lk-$(CONFIG_OP6893_CCCI_NVRAM_CACHE) += ccci-nvram-cache.o
op6893-ccci-lk-$(CONFIG_OP6893_CCCI_MD_BOOT) += ccci-md-boot.o ccci-rtc32k.o ccci-smem-clear.o
