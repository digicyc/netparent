# OpenWRT package Makefile for netparent.
#
# Drop this directory into your OpenWRT SDK as package/netparent/ and run:
#   make package/netparent/compile V=s
#
# The resulting .ipk will be in bin/packages/<arch>/base/.

include $(TOPDIR)/rules.mk

PKG_NAME:=netparent
PKG_VERSION:=0.1.0
PKG_RELEASE:=1
PKG_LICENSE:=MIT
PKG_MAINTAINER:=netparent contributors

# When integrating into the OpenWRT buildroot, point this at a tarball
# or set PKG_SOURCE / PKG_BUILD_DIR appropriately. During development
# you can copy the source tree into $(PKG_BUILD_DIR) and skip Prepare.
PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)

include $(INCLUDE_DIR)/package.mk

define Package/netparent
  SECTION:=net
  CATEGORY:=Network
  TITLE:=MQTT-driven MAC blocker
  URL:=https://example.com/netparent
  DEPENDS:=+libmosquitto-ssl +libuci +libcjson +nftables-json +ca-bundle
endef

define Package/netparent/description
  netparent subscribes to a remote MQTT broker over TLS and enables or
  disables internet access for individual LAN devices by adding their
  MAC addresses to an nftables set.
endef

define Package/netparent/conffiles
/etc/config/netparent
endef

# Copy our source tree into the build directory before compiling.
define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ../src $(PKG_BUILD_DIR)/
	$(CP) ../Makefile $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(MAKE) -C $(PKG_BUILD_DIR) \
		CC="$(TARGET_CC)" \
		CFLAGS="$(TARGET_CFLAGS) -D_GNU_SOURCE" \
		LDFLAGS="$(TARGET_LDFLAGS)" \
		PKG_CONFIG="$(PKG_CONFIG)"
endef

define Package/netparent/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/netparent $(1)/usr/sbin/netparent

	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) ../files/usr/bin/netparentctl $(1)/usr/bin/netparentctl

	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ../files/etc/init.d/netparent $(1)/etc/init.d/netparent

	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ../files/etc/config/netparent $(1)/etc/config/netparent

	$(INSTALL_DIR) $(1)/etc/netparent
endef

define Package/netparent/postinst
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] && exit 0
/etc/init.d/netparent enable
echo "netparent installed."
echo "Edit /etc/config/netparent and place your CA cert at /etc/netparent/ca.crt,"
echo "then run: /etc/init.d/netparent start"
exit 0
endef

define Package/netparent/prerm
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] && exit 0
/etc/init.d/netparent stop  2>/dev/null || true
/etc/init.d/netparent disable 2>/dev/null || true
exit 0
endef

$(eval $(call BuildPackage,netparent))
