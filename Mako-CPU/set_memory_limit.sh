#!/bin/bash
user_name=${USER}
sudo cgcreate -t ${user_name} -a ${user_name} -g memory:/${cgroup_name}
echo 10g > /sys/fs/cgroup/memory/${cgroup_name}/memory.limit_in_bytes