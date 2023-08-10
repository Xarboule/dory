#!/bin/bash

# Found the trick here :
# https://github.com/microsoft/SwitchML/blob/main/RDMAExampleClient/README.md#disabling-icrc-checking-on-nic

if [ "$#" -ne 2 ]; then
    echo -e "\nUsage : $0 <device> <--read | --write>\n\n"
    echo "To get your Mellanox device list, check : lspci -d 15b3:"
    echo -e "Example of PCI device expectef format : af:00.0\n"
    exit 1
fi

if [ "$2" = "--read" ]; then

    set -e

    sudo mstmcra $1 0x5361c.12:1 
    sudo mstmcra $1 0x5363c.12:1 
    sudo mstmcra $1 0x53614.29:1 
    sudo mstmcra $1 0x53634.29:1 
    
elif [ "$2" = "--write" ]; then

    set -e

    sudo mstmcra $1 0x5361c.12:1 0
    sudo mstmcra $1 0x5363c.12:1 0
    sudo mstmcra $1 0x53614.29:1 0
    sudo mstmcra $1 0x53634.29:1 0

else
    echo -e "\nUsage : $0 <device> <--read | --write>\n\n"
    echo "You must use --read or --write"
    echo -e "\n"
    exit 1
fi
echo "Done."
