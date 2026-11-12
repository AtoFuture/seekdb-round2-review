#!/bin/sh
set -eu
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
mkdir -p /var/lib/minivectordb-docker /var/lib/minivectordb-docker-client
exec dockerd --data-root=/var/lib/minivectordb-docker --host=unix:///var/run/docker.sock
