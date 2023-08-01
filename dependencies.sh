apt-get update
apt-get -y install apt-utils
apt-get -y install libibverbs-dev libmemcached-dev python3 python3-pip ninja-build clang lld clang-format
apt-get -y install net-tools iputils-ping
apt-get -y install vim tmux git memcached libevent-dev libhugetlbfs-dev libgtest-dev libnuma-dev numactl libgflags-dev

#conan must be 1.XX, and not 2.XX. 
pip3 install conan==1.60.1
conan profile new default --detect
conan profile update settings.compiler.libcxx=libstdc++11 default
conan profile update settings.compiler.cppstd=17 default

#installing the lastest version of cmake 
apt-get update
apt-get install -y software-properties-common wget
wget -O - https://apt.kitware.com/keys/kitware-archive-latest.asc 2>/dev/null | gpg --dearmor - | tee /etc/apt/trusted.gpg.d/kitware.gpg >/dev/null
apt-add-repository 'deb https://apt.kitware.com/ubuntu/ bionic main'
apt-get install -y cmake

cd /usr/src/gtest && cmake CMakeLists.txt && make && make install

#installing the Mellanox software stack 
cd /
wget http://content.mellanox.com/ofed/MLNX_OFED-${MOFED_VER}/MLNX_OFED_LINUX-${MOFED_VER}-${OS_VER}-${PLATFORM}.tgz 
tar -xvf MLNX_OFED_LINUX-${MOFED_VER}-${OS_VER}-${PLATFORM}.tgz 
MLNX_OFED_LINUX-${MOFED_VER}-${OS_VER}-${PLATFORM}/mlnxofedinstall --user-space-only --without-fw-update -q
cd ..
rm -rf ${MOFED_DIR} 
rm -rf *.tgz

