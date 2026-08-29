#!/bin/bash

#
# Copyright (c) 2021.  Jefferson Science Associates, LLC.
# Subject to the terms in the LICENSE file found in the top-level directory.
# Author gyurjyan
#

<#-- export MALLOC_ARENA_MAX=2 -->
<#-- export MALLOC_MMAP_THRESHOLD_=131072 -->
<#-- export MALLOC_TRIM_THRESHOLD_=131072 -->
<#-- export MALLOC_TOP_PAD_=131072 -->
<#-- export MALLOC_MMAP_MAX_=65536 -->
<#-- export MALLOC_MMAP_MAX_=65536 -->

ulimit -u 7000

<#if farm.javaOpts??>
export JAVA_OPTS="${farm.javaOpts}"
</#if>

export ERSAP_HOME="${ersap.dir}"
export ERSAP_MONITOR_FE="${ersap.monitorFE!"129.57.70.24%9000_java"}"
export CLAS12DIR="${clas12.dir}"
export ERSAP_USER_DATA="${user_data.dir}"

# Set the following env variables if we are at the JLAB
if ping -w 1 -c 1 129.57.32.100 &> /dev/null
then
export CCDB_CONNECTION=mysql://clas12reader@clasdb-farm.jlab.org/clas12
export RCDB_CONNECTION=mysql://rcdb@clasdb-farm.jlab.org/rcdb
fi

<#-- "$ERSAP_HOME/bin/kill-dpes" -->

<#-- sleep $[ ( $RANDOM % 20 )  + 1 ]s -->

${farm.command}
