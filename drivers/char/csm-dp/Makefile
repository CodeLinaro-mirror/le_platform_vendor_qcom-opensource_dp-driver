#
# Makefile for the Qualcomm Technologies, Inc. CSM DP driver
#
CONFIG_CSM_DP ?= m
obj-$(CONFIG_CSM_DP) += csm_dp.o

csm_dp-objs := csm_dp_core.o csm_dp_cdev.o csm_dp_mhi.o csm_dp_mem.o
csm_dp-objs += csm_dp_debugfs.o

MHI_BUILD_ROOT ?= $(src)/../../../../../mhi-host
KBUILD_EXTRA_SYMBOLS ?= $(MHI_BUILD_ROOT)/drivers/bus/mhi/Module.symvers

ccflags-y := -I$(MHI_BUILD_ROOT)/include

######### Strip this line and below when upstreaming ###########

### extra stuff for stand alone module.

DP_MODULE_VERSION ?= $(shell echo ENG-$$(whoami; date +%D) | sed "s/ /-/g")
ccflags-y += -DDP_MODULE_VERSION='"$(DP_MODULE_VERSION)"'

# some kernels don't set it for out-of-tree modules
# as result, <> style #include for uapi/ don't work
subdir-ccflags-y += -I$(src)/../../../include/uapi
