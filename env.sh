#!/bin/bash

# ERSAP runtime paths (not defined in Dockerfile)
export ERSAP_HOME=$(pwd)/ersap
export ERSAP_USER_DATA=/global/cfs/cdirs/amsc016/haidis/ersap-data

# Node IP -- first non-loopback address; used as recv-ip in services.yaml
export NODE_IP=$(hostname -I | awk '{print $1}')
