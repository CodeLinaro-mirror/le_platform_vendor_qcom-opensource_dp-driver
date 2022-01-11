#
# Makefile for the Qualcomm Technologies, Inc. FSM DP driver
#
obj-$(CONFIG_FSM_DP) += fsm_dp.o

fsm_dp-objs := fsm_dp_core.o fsm_dp_cdev.o fsm_dp_mhi.o fsm_dp_mem.o
fsm_dp-objs += fsm_dp_debugfs.o

######### Strip this line and below when upstreaming ###########

### extra stuff for stand alone module.

# some kernels don't set it for out-of-tree modules
# as result, <> style #include for uapi/ don't work
subdir-ccflags-y += -I$(src)/../../../include/uapi
