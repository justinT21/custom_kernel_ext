################################################################################
#
# tracemark
#
################################################################################

TRACEMARK_VERSION = 1.0
TRACEMARK_SITE = $(BR2_EXTERNAL_CUSTOM_RV32_PATH)/src/tracemark
TRACEMARK_SITE_METHOD = local
TRACEMARK_FLAT_STACKSIZE = 0x2000
# Installs trace_marker.h into the sysroot, so other packages can
# #include <trace_marker.h> (add tracemark to their _DEPENDENCIES)
TRACEMARK_INSTALL_STAGING = YES

define TRACEMARK_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) $(TARGET_CONFIGURE_OPTS) -C $(@D)
endef

define TRACEMARK_INSTALL_STAGING_CMDS
	$(INSTALL) -D -m 0644 $(@D)/trace_marker.h $(STAGING_DIR)/usr/include/trace_marker.h
endef

define TRACEMARK_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/tracemark $(TARGET_DIR)/usr/bin/tracemark
endef

$(eval $(generic-package))
