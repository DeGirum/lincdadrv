# SPDX-License-Identifier: GPL-2.0
# Copyright(c) 2020 DeGirum Corp., Egor Pomozov.
#
# CDA linux driver mem blocks/mem maps and interrupt request handler
#
# This program is free software; you can redistribute it and/or modify it
# under the terms and conditions of the GNU General Public License,
# version 2, as published by the Free Software Foundation.
#
TARGET_MODULE := cdapci
THIS_MKFILE := $(lastword $(MAKEFILE_LIST))
THIS_MKFILE_DIR := $(dir $(abspath $(THIS_MKFILE)))

TARGET_VERSION := $(shell grep "^MODULE_VERSION" $(THIS_MKFILE_DIR)/cdadrv.c | cut -c 17-23)
BUILDDIR ?= /lib/modules/$(shell uname -r)/build

PREFIX_DRIVER_PATH ?=
SUFFIX_DRIVER_PATH ?=
ifneq ($(PREFIX_DRIVER_PATH)$(SUFFIX_DRIVER_PATH),)
DRIVER_PATH := $(PREFIX_DRIVER_PATH)/$(TARGET_MODULE)/$(TARGET_VERSION)/$(SUFFIX_DRIVER_PATH)
else
DRIVER_PATH ?= $(THIS_MKFILE_DIR)
endif
IS_SYSTEMD_USED=$(shell pidof systemd && echo "systemd" || echo "other")
IS_THERE_CDA_GROUP=$(shell getent group dg_orca && echo "yes" || echo "no")

IS_SB_EN ?= $(shell sudo mokutil --sb-state | grep -ci "enabled")
KEY_DER_PATH ?= /var/lib/shim-signed/mok/MOK.der
KEY_BASENAME := $(basename $(KEY_DER_PATH))
KEY_PRIV_PATH ?= $(KEY_BASENAME).priv
IS_KEY_PRESENT=$(shell test -f $(KEY_DER_PATH) && echo "yes" || echo "no")
IS_PKEY_PRESENT=$(shell test -f $(KEY_PRIV_PATH) && echo "yes" || echo "no")
KEY_IN_MOK=$(shell sudo mokutil --test-key $(KEY_DER_PATH) | grep -ci "is already enrolled")
KDER_SIGN_PIN?=degirum
DG_VID=1f0d
XL_VID=10ee
DG_GROUP=dg_orca
VIDS="$(DG_VID) $(XL_VID)"

UDEV_RULE0='SUBSYSTEM=="cda", MODE="0666", GROUP="$(DG_GROUP)"'
UDEV_RULE1='SUBSYSTEM=="cda", ACTION=="add", RUN+="/usr/local/bin/force_usr_mode.sh"'
#
FORCE_USR_MODE0="\#!/bin/sh"
FORCE_USR_MODE1='VIDS=$(VIDS);'
FORCE_USR_MODE2='for vid in $$VIDS;'
FORCE_USR_MODE3='do for d in $$(dirname -- $$(find /sys/devices/* -name "vendor" -exec grep -H "$$vid" {} \;));'
FORCE_USR_MODE4='do /bin/chmod ug+rw $$d/resource* ; /bin/chown root:$(DG_GROUP) $$d/resource* ;'
FORCE_USR_MODE5='for dd in $$(dirname -- $$(find $$d/cda/* -name "mmap")); do /bin/chmod ug+rw $$dd/mmap ; /bin/chown root:$(DG_GROUP) $$dd/mmap ; done'
FORCE_USR_MODE6='done'
FORCE_USR_MODE7='done'
ifneq ($(KERNELRELEASE),)
	obj-m := $(TARGET_MODULE).o
	$(TARGET_MODULE)-objs := cdadrv.o cdamem.o cdares.o
endif
.PHONY: all
all:
	$(MAKE) -C $(BUILDDIR) M=$(THIS_MKFILE_DIR) modules
clean:
	$(MAKE) -C $(BUILDDIR) M=$(THIS_MKFILE_DIR) clean
sign: export KBUILD_SIGN_PIN=$(KDER_SIGN_PIN)
sign:
ifeq ($(IS_SB_EN),1)
ifneq ($(IS_KEY_PRESENT),no)
	$(shell sudo --preserve-env=KBUILD_SIGN_PIN -E /usr/src/linux-headers-$$(uname -r)/scripts/sign-file sha512 $(KEY_PRIV_PATH) $(KEY_DER_PATH) $(DRIVER_PATH)/cdapci.ko )
else
	$(error "Key is not present")
endif
else
	$(info "No need to sign the driver")
endif
preinstall:
ifeq ($(IS_SB_EN),1)
ifeq ($(IS_KEY_PRESENT),no)
	$(warning "No MOK key. Create it")
	sudo openssl req -new -x509 -newkey rsa:2048 -keyout $(KEY_PRIV_PATH) -outform DER -out $(KEY_DER_PATH) -days 36500 -subj "/CN=cdapci module signing key/" -passout pass:$(KDER_SIGN_PIN)
else
	$(info "MOK key presented")
endif
ifeq ($(KEY_IN_MOK),0)
	$(warning "Key should be installed in MOK. Please reboot the system to complete it.")
	@sudo mokutil --import $(KEY_DER_PATH)
else
	$(info "Key is installed in MOK")
endif
	$(MAKE) sign KEY_DER_PATH=$(KEY_DER_PATH) KEY_PRIV_PATH=$(KEY_PRIV_PATH)
else
	$(info "Security Boot is not enabled. Driver signing is not required.")
endif

postinstall:
	@sudo -E depmod
ifneq ($(IS_SYSTEMD_USED),other)
	@echo $(TARGET_MODULE) | sudo -E tee -i /etc/modules-load.d/cdapci.conf > /dev/null
else
	$(warning "No module autostart on reboot")
endif
ifeq ($(IS_THERE_CDA_GROUP),no)
	$(warning "No group dg_orca. Creating it.")
	@sudo -E groupadd $(DG_GROUP)
endif
	$(warning "Adding users to dg_orca.")
	@for ID in `cat /etc/passwd | grep /home | cut -d ':' -f1` ; do (sudo -E usermod -a -G $(DG_GROUP) $$ID) ; done

	@echo $(FORCE_USR_MODE0) | sudo -E tee -i /usr/local/bin/force_usr_mode.sh > /dev/null
	@echo $(FORCE_USR_MODE1) | sudo -E tee -a /usr/local/bin/force_usr_mode.sh > /dev/null
	@echo $(FORCE_USR_MODE2) | sudo -E tee -a /usr/local/bin/force_usr_mode.sh > /dev/null
	@echo $(FORCE_USR_MODE3) | sudo -E tee -a /usr/local/bin/force_usr_mode.sh > /dev/null
	@echo $(FORCE_USR_MODE4) | sudo -E tee -a /usr/local/bin/force_usr_mode.sh > /dev/null
	@echo $(FORCE_USR_MODE5) | sudo -E tee -a /usr/local/bin/force_usr_mode.sh > /dev/null
	@echo $(FORCE_USR_MODE6) | sudo -E tee -a /usr/local/bin/force_usr_mode.sh > /dev/null
	@echo $(FORCE_USR_MODE7) | sudo -E tee -a /usr/local/bin/force_usr_mode.sh > /dev/null
	@sudo -E chmod +x /usr/local/bin/force_usr_mode.sh > /dev/null
	@echo $(UDEV_RULE0) | sudo -E tee -i /etc/udev/rules.d/66-cdapci.rules > /dev/null
	@echo $(UDEV_RULE1) | sudo -E tee -a /etc/udev/rules.d/66-cdapci.rules > /dev/null
	@sudo -E udevadm control --reload-rules

install: preinstall
	sudo -E $(MAKE) -C $(BUILDDIR) M=$(THIS_MKFILE_DIR) modules_install
	$(MAKE) -f $(THIS_MKFILE) postinstall

preuninstall:
ifneq ($(IS_SYSTEMD_USED),other)
	@sudo -E rm -f /etc/modules-load.d/cdapci.conf
endif
	-sudo -E modprobe -r $(TARGET_MODULE)
	@sudo -E rm -f /usr/local/bin/force_usr_mode.sh
	@sudo -E rm -f /etc/udev/rules.d/66-cdapci.rules

uninstall: preuninstall
	@sudo -E rm -f $(shell modinfo -n $(TARGET_MODULE))
	@sudo -E depmod

dkms: clean
	@sudo -E sed -e 's/@_PACKAGE_VERSION@/$(TARGET_VERSION)/'\
	             -e 's/@_PACKAGE_NAME@/$(TARGET_MODULE)/' dkms.conf.tmpl >dkms.conf
	@sudo -E mkdir -p /usr/src/$(TARGET_MODULE)-$(TARGET_VERSION)
	@sudo -E cp -Rf * /usr/src/$(TARGET_MODULE)-$(TARGET_VERSION)/
	@sudo -E dkms add -m $(TARGET_MODULE) -v $(TARGET_VERSION)
	@sudo -E dkms install $(TARGET_MODULE) -v $(TARGET_VERSION)

dkms-clean:
	@$(foreach r,  $(shell ls /usr/src/ | grep $(TARGET_MODULE) | sed 's|-|/|g'), sudo -E dkms remove -m $r --all;)
	-sudo -E rm -rf /usr/src/$(TARGET_MODULE)-$(TARGET_VERSION)

