#
# Makefile for the Qualcomm Technologies, Inc. FSM DP driver
#
CONFIG_FSM_DP ?= m
obj-$(CONFIG_FSM_DP) += fsm_dp.o

fsm_dp-objs := fsm_dp_core.o fsm_dp_cdev.o fsm_dp_mhi.o fsm_dp_mem.o
fsm_dp-objs += fsm_dp_debugfs.o

MHI_BUILD_ROOT := $(src)/../../../../../mhi-host
KBUILD_EXTRA_SYMBOLS ?= $(MHI_BUILD_ROOT)/drivers/bus/mhi/Module.symvers

ccflags-y := -I$(MHI_BUILD_ROOT)/include

######### Strip this line and below when upstreaming ###########

### extra stuff for stand alone module.

DP_MODULE_VERSION ?= $(shell echo ENG-$$(whoami; date +%D) | sed "s/ /-/g")
ccflags-y += -DDP_MODULE_VERSION='"$(DP_MODULE_VERSION)"'

# some kernels don't set it for out-of-tree modules
# as result, <> style #include for uapi/ don't work
subdir-ccflags-y += -I$(src)/../../../include/uapi
