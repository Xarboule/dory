./build.py -b debug crash-consensus
crash-consensus/libgen/export.sh gcc-debug
crash-consensus/demo/using_conan_fully/build.sh gcc-debug
crash-consensus/experiments/build.sh
crash-consensus/experiments/liquibook/build.sh
export LD_LIBRARY_PATH=$HOME/dory/crash-consensus/experiments/exported:$LD_LIBRARY_PATH
echo "export LD_LIBRARY_PATH=$HOME/dory/crash-consensus/experiments/exported:$LD_LIBRARY_PATH" >> ~/.bashrc

cd demo/
./build.sh
