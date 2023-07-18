FROM ubuntu:bionic

ENV MOFED_VER 23.04-1.1.3.0
ENV OS_VER ubuntu18.04
ENV PLATFORM x86_64

RUN apt-get update
RUN apt-get -y install apt-utils
RUN apt-get -y install libibverbs-dev libmemcached-dev python3 python3-pip ninja-build clang lld clang-format
RUN apt-get -y install net-tools iputils-ping

RUN pip3 install conan==1.60.1
RUN conan profile new default --detect
RUN conan profile update settings.compiler.libcxx=libstdc++11 default

RUN apt-get update && apt-get -y install vim tmux git memcached libevent-dev libhugetlbfs-dev libgtest-dev libnuma-dev numactl libgflags-dev

RUN apt-get install -y openssh-server
#RUN mkdir /var/run/sshd
#RUN sed -ri 's/UsePAM yes/#UsePAM yes/g' /etc/ssh/sshd_config

RUN apt remove --purge --auto-remove cmake
RUN apt update
RUN apt install -y software-properties-common lsb-release 
RUN apt clean all
RUN wget -O - https://apt.kitware.com/keys/kitware-archive-latest.asc 2>/dev/null | gpg --dearmor - | tee /etc/apt/trusted.gpg.d/kitware.gpg >/dev/null
RUN apt-add-repository "deb https://apt.kitware.com/ubuntu/ $(lsb_release -cs) main"
RUN apt update
RUN apt install -y cmake
RUN cd /usr/src/gtest && cmake CMakeLists.txt && make && make install

RUN wget --quiet http://content.mellanox.com/ofed/MLNX_OFED-${MOFED_VER}/MLNX_OFED_LINUX-${MOFED_VER}-${OS_VER}-${PLATFORM}.tgz && \
    tar -xvf MLNX_OFED_LINUX-${MOFED_VER}-${OS_VER}-${PLATFORM}.tgz && \
    MLNX_OFED_LINUX-${MOFED_VER}-${OS_VER}-${PLATFORM}/mlnxofedinstall --user-space-only --without-fw-update -q && \
    cd .. && \
    rm -rf ${MOFED_DIR} && \
    rm -rf *.tgz



#ENV DOCKERUSER=devuser
#RUN adduser --disabled-password --shell /bin/bash --gecos "Docker User" $DOCKERUSER
#RUN echo "$DOCKERUSER:<dockeruser-password>" |chpasswd

#EXPOSE 22

#CMD    ["/usr/sbin/sshd", "-D"]

RUN mkdir /mu
COPY sleep.sh /mu/sleep.sh
RUN chmod +x /mu/sleep.sh
CMD /mu/sleep.sh