# DKMS Makefile for rt2800usb (Ralink RT5370 USB WiFi)
# Built from Linux 5.15.0 kernel sources against Jetson OOT kernel headers

KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build

# Core objects for rt2x00lib
rt2x00lib-y := rt2x00dev.o rt2x00mac.o rt2x00config.o rt2x00queue.o rt2x00link.o \
               rt2x00firmware.o rt2x00leds.o rt2x00crypto.o

obj-m += rt2x00lib.o
obj-m += rt2x00usb.o
obj-m += rt2800lib.o
obj-m += rt2800usb.o

# Enable required features
ccflags-y := -DCONFIG_RT2X00_LIB_FIRMWARE \
             -DCONFIG_RT2X00_LIB_LEDS \
             -DCONFIG_RT2X00_LIB_CRYPTO \
             -DCONFIG_RT2X00_LIB_USB \
             -DCONFIG_RT2800_LIB \
             -DCONFIG_RT2800USB \
             -DCONFIG_RT2800USB_RT53XX

all:
	$(MAKE) -C $(KDIR) M=$(shell pwd) modules

clean:
	$(MAKE) -C $(KDIR) M=$(shell pwd) clean
