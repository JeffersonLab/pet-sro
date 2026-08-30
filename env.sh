#!/bin/bash

# ERSAP runtime paths (not defined in Dockerfile)
export ERSAP_HOME=$(pwd)/ersap
export ERSAP_USER_DATA=/global/cfs/cdirs/amsc016/haidis/ersap-data

# Node IP -- first public IPv4 address (skips RFC-1918 private ranges and
# IPv6), used as recv-ip in services.yaml.
export NODE_IP=$(hostname -I | tr ' ' '\n' | grep -E '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$' | grep -v '^10\.' | grep -v '^172\.\(1[6-9]\|2[0-9]\|3[01]\)\.' | grep -v '^192\.168\.' | head -1)

# Substitute NODE_IP (and any other exported vars) into the services YAML
# in-place. The redirect-to-tmp + mv pattern avoids the truncation that
# would happen with `envsubst < file > file`.
envsubst < "$ERSAP_USER_DATA/config/pet_services.yaml" > /tmp/services.yaml \
    && mv /tmp/services.yaml "$ERSAP_USER_DATA/config/pet_services.yaml"
