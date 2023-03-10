#!/bin/sh

# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.

op=$1
DRACUT_CONF_FILE=/etc/dracut.conf.d/dp-driver.conf

if [ "$op" == "build" ];
then
    echo 'omit_drivers+=" csm_dp "' > $DRACUT_CONF_FILE
elif [ "$op" == "remove" ];
then
	rm -f $DRACUT_CONF_FILE
	rmmod csm_dp
fi
