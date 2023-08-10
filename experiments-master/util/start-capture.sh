#!/bin/bash

if [ $# -eq 0 ]
  then
    echo "Usage : $0 <file-name.pcap>"
    exit 1
fi

echo "==> Capture in /tmp/traces..."

docker run --rm -it -v /dev/infiniband:/dev/infiniband -v /tmp/traces:/tmp/traces --net=host --privileged mellanox/tcpdump-rdma tcpdump -i mlx5_0 -s 100 -w /tmp/traces/$1

echo "Copying capture..."

cp /tmp/traces/$1 .
