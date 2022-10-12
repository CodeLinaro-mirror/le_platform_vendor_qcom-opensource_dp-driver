obj-m += dp-driver/drivers/char/fsm-dp/

KVER=$(shell uname -r)

M ?= $(shell pwd)

all:
	make -C /lib/modules/$(KVER)/build M=$(M) /DP_MODULE_VERSION="$(DP_MODULE_VERSION)" modules
clean:
	make -C /lib/modules/$(KVER)/build M=$(M) clean

