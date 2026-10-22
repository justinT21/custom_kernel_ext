################################################################################
#
# qemu-tracewin (host only)
#
################################################################################

HOST_QEMU_TRACEWIN_VERSION = 1.0
HOST_QEMU_TRACEWIN_SITE = $(BR2_EXTERNAL_CUSTOM_RV32_PATH)/src/qemu-tracewin
HOST_QEMU_TRACEWIN_SITE_METHOD = local
HOST_QEMU_TRACEWIN_LICENSE = MIT
HOST_QEMU_TRACEWIN_LICENSE_FILES = LICENSE
HOST_QEMU_TRACEWIN_DEPENDENCIES = host-qemu host-libglib2

define HOST_QEMU_TRACEWIN_BUILD_CMDS
	$(HOST_MAKE_ENV) $(MAKE) $(HOST_CONFIGURE_OPTS) -C $(@D)
endef

define HOST_QEMU_TRACEWIN_INSTALL_CMDS
	$(INSTALL) -D -m 0755 $(@D)/libtracewin.so \
		$(HOST_DIR)/lib/qemu-plugins/libtracewin.so
endef

$(eval $(host-generic-package))
