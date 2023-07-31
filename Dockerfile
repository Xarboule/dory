FROM ubuntu:bionic

ENV MOFED_VER 23.04-1.1.3.0
ENV OS_VER ubuntu18.04
ENV PLATFORM x86_64

ENV DEBIAN_FRONTEND=noninteractive

#installing the dependencies 
RUN apt-get update
RUN apt-get -y install apt-utils
RUN apt-get -y install libibverbs-dev libmemcached-dev python3 python3-pip ninja-build clang lld clang-format
RUN apt-get -y install net-tools iputils-ping
RUN apt-get -y install vim tmux git memcached libevent-dev libhugetlbfs-dev libgtest-dev libnuma-dev numactl libgflags-dev

#conan must be 1.XX, and not 2.XX. 
RUN pip3 install conan==1.60.1
RUN conan profile new default --detect
RUN conan profile update settings.compiler.libcxx=libstdc++11 default
RUN conan profile update settings.compiler.cppstd=17 default

#installing the lastest version of cmake 
RUN apt-get update
RUN apt-get install -y software-properties-common wget
RUN wget -O - https://apt.kitware.com/keys/kitware-archive-latest.asc 2>/dev/null | gpg --dearmor - | tee /etc/apt/trusted.gpg.d/kitware.gpg >/dev/null
RUN apt-add-repository 'deb https://apt.kitware.com/ubuntu/ bionic main'
RUN apt-get install -y cmake

RUN cd /usr/src/gtest && cmake CMakeLists.txt && make && make install

#installing the Mellanox software stack 
RUN wget --quiet http://content.mellanox.com/ofed/MLNX_OFED-${MOFED_VER}/MLNX_OFED_LINUX-${MOFED_VER}-${OS_VER}-${PLATFORM}.tgz 
RUN tar -xvf MLNX_OFED_LINUX-${MOFED_VER}-${OS_VER}-${PLATFORM}.tgz 
RUN MLNX_OFED_LINUX-${MOFED_VER}-${OS_VER}-${PLATFORM}/mlnxofedinstall --user-space-only --without-fw-update -q
RUN cd ..
RUN rm -rf ${MOFED_DIR} 
RUN rm -rf *.tgz




#simple way to make sure the container never ends 
RUN mkdir /mu
COPY sleep.sh /mu/sleep.sh
RUN chmod +x /mu/sleep.sh
CMD /mu/sleep.sh
