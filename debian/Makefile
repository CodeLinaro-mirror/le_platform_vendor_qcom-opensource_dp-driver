obj-m += drivers/char/csm-dp/

KERNELRELEASE=$(shell uname -r)

M ?= $(shell pwd)

all:
	make -C /lib/modules/$(KERNELRELEASE)/build M=$(M) DP_MODULE_VERSION="$(DP_MODULE_VERSION)" modules
clean:
	make -C /lib/modules/$(KERNELRELEASE)/build M=$(M) clean
