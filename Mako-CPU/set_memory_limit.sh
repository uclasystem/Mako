#!/bin/bash
user_name=${USER}
sudo cgcreate -t ${user_name} -a ${user_name} -g memory:/memctl
echo 10g > /sys/fs/cgroup/memory/memctl/memory.limit_in_bytes