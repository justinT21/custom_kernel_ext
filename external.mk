#LINUX_MAKE_ENV += KCFLAGS="-march=rv32imafdc_zicsr_zifencei_zba_zbb_zbs -mabi=ilp32d"
#OPENSBI_MAKE_ENV += PLATFORM_RISCV_ISA="rv32imafdc_zicsr_zifencei_zba_zbb_zbs"

include $(sort $(wildcard $(BR2_EXTERNAL_CUSTOM_RV32_PATH)/package/*/*.mk))

# QEMU TCG plugins, for instruction/memory tracing (docs/tracing.md).
# Buildroot configures host-qemu with --disable-plugins, but HOST_QEMU_OPTS
# comes after it on the configure line, so this wins. QEMU's `make install`
# doesn't install the plugins, so copy them to host/lib/qemu-plugins/.
HOST_QEMU_OPTS += --enable-plugins

define HOST_QEMU_INSTALL_PLUGINS
	mkdir -p $(HOST_DIR)/lib/qemu-plugins
	find $(@D)/build/contrib/plugins $(@D)/build/tests/tcg/plugins \
		-maxdepth 1 -name 'lib*.so' -exec cp -f {} $(HOST_DIR)/lib/qemu-plugins/ \;
	test -e $(HOST_DIR)/lib/qemu-plugins/libexeclog.so
endef
HOST_QEMU_POST_INSTALL_HOOKS += HOST_QEMU_INSTALL_PLUGINS
