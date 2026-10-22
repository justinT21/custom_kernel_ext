################################################################################
#
# memhog
#
################################################################################

MEMHOG_VERSION = 1.0
MEMHOG_SITE = $(BR2_EXTERNAL_CUSTOM_RV32_PATH)/src/memhog
MEMHOG_SITE_METHOD = local
# Stack size written into the FLAT header (no-MMU stacks can't grow)
MEMHOG_FLAT_STACKSIZE = 0x4000

define MEMHOG_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) $(TARGET_CONFIGURE_OPTS) -C $(@D)
endef

define MEMHOG_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/memhog $(TARGET_DIR)/usr/bin/memhog
endef

$(eval $(generic-package))
