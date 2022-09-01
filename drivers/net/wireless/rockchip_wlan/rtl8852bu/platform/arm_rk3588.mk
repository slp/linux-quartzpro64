ifeq ($(CONFIG_PLATFORM_ARM_RK3588), y)
# CONFIG_RTKM - n/m/y for not support / standalone / built-in
CONFIG_RTKM = m
EXTRA_CFLAGS += -DCONFIG_LITTLE_ENDIAN
EXTRA_CFLAGS += -DCONFIG_IOCTL_CFG80211 -DRTW_USE_CFG80211_STA_EVENT
EXTRA_CFLAGS += -DCONFIG_RADIO_WORK
EXTRA_CFLAGS += -DCONFIG_CONCURRENT_MODE
ifeq ($(shell test $(CONFIG_RTW_ANDROID) -ge 11; echo $$?), 0)
EXTRA_CFLAGS += -DCONFIG_IFACE_NUMBER=3
#EXTRA_CFLAGS += -DCONFIG_SEL_P2P_IFACE=1
#EXTRA_CFLAGS += -DCONFIG_PLATFORM_ROCKCHIPS
endif

ARCH := arm64
CROSS_COMPILE := /home/android_sdk/Rockchip/Rk3588/prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-


KSRC := /home/android_sdk/Rockchip/Rk3588/kernel

ifeq ($(CONFIG_PCI_HCI), y)
EXTRA_CFLAGS += -DCONFIG_PLATFORM_OPS
_PLATFORM_FILES := platform/platform_linux_pc_pci.o
OBJS += $(_PLATFORM_FILES)
# Core Config
CONFIG_MSG_NUM = 128
EXTRA_CFLAGS += -DCONFIG_MSG_NUM=$(CONFIG_MSG_NUM)
EXTRA_CFLAGS += -DCONFIG_RXBUF_NUM_1024
EXTRA_CFLAGS += -DCONFIG_TX_SKB_ORPHAN
EXTRA_CFLAGS += -DCONFIG_DIS_DYN_RXBUF
# PHL Config
EXTRA_CFLAGS += -DRTW_WKARD_98D_RXTAG
endif

endif
