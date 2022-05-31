# Mako README

*Mako* is a low-pause, high-throughput garbage collector designed for memory-disaggregated datacenters. Key to Mako’s success is its ability to offload both tracing and evacuation onto memory servers and run these tasks concurrently when the CPU server executes mutator threads.  Mako achieves *~12ms* at the
90th-percentile pause time and outperforms Shenandoah by an average of 3 times in throughput. 

Please read [our PLDI'22 paper](http://web.cs.ucla.edu/~harryxu/papers/mako-pldi22.pdf) for the design details. 

## Setup Environments

- **Hardware: Intel servers with InfiniBand**
- **Kernel environments: Linux-4.11-rc8**
- **OS versions: CentOS 7.5(7.6) with MLNX-OFED 4.3(4.5)**
- **Run-time environments: OpenJDK 13**
- **GNU environments: GCC 4.8 to GCC 5.5, GLIBC 2.27**
- **Code license: The GNU General Public License (GPL)**

## *Mako*’s Codebase

*Mako* contains the following three components:

- the Linux kernel with functionalities required by *Mako - kernel*
- the CPU-server Java Virtual Machine - *Mako*-CPU
- the Memory-server Java Virtual Machine - *Mako*-Memory

## Deploying *Mako*

When deploying *Mako*, install the three components in the following order: the kernel on the CPU server and the Memory server, the Mako-CPU JVM on the CPU server, and the Mako-Memory on the memory server. Finally, connect the CPU server with the memory server before running applications.

### CPU Server Kernel Installation

We first discuss how to build and install the kernel.

- Modify *grub* and set `transparent_hugepage` to *`madvise`*:
    
    ```bash
    sudo vim /etc/default/grub
    + transparent_hugepage=madvise
    ```
    
- Install the kernel and restart the machine on both CPU server and memory servers:
    
    ```bash
    cd Mako/kernel
    sudo ./build_kernel.sh build
    # `grub_boot_verion` may need to be changed to boot with the correct kernel
    # use `uname -a` after reboot to see whether the correct kernel is booted
    sudo ./build_kernel.sh install
    sudo reboot
    ```
    
- Install the MLNX-OFED driver. We download the `MLNX_OFED_LINUX-4.5-1.0.1.0-rhel7.6-x86_64`, and install it against our newly built kernel:
    
    ```bash
    # check CentOS version, and select the correct driver to install!
    # Download link can be find here: https://www.mellanox.com/products/infiniband-drivers/linux/mlnx_ofed
    wget https://content.mellanox.com/ofed/MLNX_OFED-4.5-1.0.1.0/MLNX_OFED_LINUX-4.5-1.0.1.0-rhel7.6-x86_64.tgz
    tar xzf MLNX_OFED_LINUX-4.5-1.0.1.0-rhel7.6-x86_64.tgz
    cd MLNX_OFED_LINUX-4.5-1.0.1.0-rhel7.6-x86_64
    sudo yum install -y createrepo rpm-build pciutils gtk2 atk cairo libxml2-python tcsh lsof tcl tk net-tools
    sudo ./mlnxofedinstall --add-kernel-support
    sudo /etc/init.d/openibd restart
    ```
    
    After installing the OFED driver, please confirm the RDMA works well between the CPU server and the memory server.
    
- Build and install the RDMA module on the CPU server
    
    ```cpp
    // Add the IP of memory server into
    // Mako/kernel/semeru/semeru_cpu.c
    // e.g., the Inﬁniband IPs of the memory server is 1.2.3.4
    char *mem_server_ip[] = {"1.2.3.4"};
    uint16_t mem_server_port = 9400;
    ```
    
    ```bash
    # Then build the Semeru RDMA module
    cd Mako/kernel/semeru
    make
    ```
    

### JDK Build Dependencies Installation

To build JDK, some packages needs to be installed first:

```bash
sudo yum groupinstall "Development Tools" -y
sudo yum install libXtst-devel libXt-devel libXrender-devel libXrandr-devel libXi-devel cups-devel fontconfig-devel alsa-lib-devel -y
```

### Building *Mako*-CPU and *Mako*-Memory

The next step is to install the JVM on each server.

- Download OpenJDK 12 as the boot JDK used for JDK compilation. Assume that boot JDK is under the path: `${HOME}/jdk-12`
- Change the IP address on the Memory server.
    
    ```cpp
    // E.g., memory server' InfiniBand interface IP is 2.3.4.5.
    // Change the IP address in file:
    // Mako/Mako-Memory/src/hotspot/share/utilities/globalDefinitions.hpp
    static const char cur_mem_server_ip[] = "2.3.4.5";
    static const char cur_mem_server_port[]= "9400";
    ```
    
- Build the JVMs
    
    ```bash
    cd Mako/Mako-CPU
    bash ./configure --with-debug-level=release --with-target-bits=64 --disable-dtrace --with-boot-jdk=${HOME}/jdk-12
    make CONF=linux-x86_64-server-release
    
    cd Mako/Mako-Memory
    bash ./configure --with-debug-level=release --with-target-bits=64 --disable-dtrace --with-boot-jdk=${HOME}/jdk-12 --with-extra-cxxflags="-lrdmacm -libverbs" --with-extra-ldflags="-lrdmacm -libverbs" 
    make CONF=linux-x86_64-server-release
    ```
    

## Running Applications

To run applications, we first need to connect the CPU server with memory servers. Next, we mount the remote memory pools as a swap partition on the CPU server. When the application uses more memory than the limit set by *cgroup*, its data will be swapped out to the remote memory via RDMA. 

- Launch memory server
    
    ```bash
    cd ~/Mako/Mako-Memory
    javac Case1.java
    tmux
    ./run_mem_server.sh
    ```
    
- Connect the CPU server with memory servers
    
    ```bash
    # @CPU server
    cd ~/Mako/kernel/semeru
    sudo ./manage_semeru_frontswap_module.sh semeru
    
    # To close the swap partition, do the following:
    # @CPU server
    sudo ./manage_semeru_frontswap_module.sh close_semeru
    ## restart the CPU server before installing again, or there may be errors.
    ```
    

### Example application: Dacapo Tradesoap

- Download dacapo benchmark from [https://dacapo-bench.org/](https://dacapo-bench.org/)
- Move it to the Mako folder
- Set a CPU server cache size limit (e.g. 25%)
    
    ```bash
    # The shellscript will create a cgroup, named as memctl.
    # @CPU server
    cd ~/Mako/Mako-CPU
    ./set_memory_limit.sh
    ```
    
- Change to Mako-CPU and run the example application
    
    ```bash
    cd ~/Mako/Mako-CPU
    ./example.sh
    ```
