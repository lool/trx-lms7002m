#!/bin/bash
# Copyright (C) 2015-2017 Amarisoft/LimeMicroSystems
# Install script version 2017-02-10


DIR=$(cd $(dirname $0) && pwd)
DST=$(cd $1 && pwd)

if [ ! -d "$DST" ] ; then
    if [ "$DST" = "" ] ; then
        echo "Usage:"
        echo "> $0 <eNB path>"
    else
        echo "eNB directory '$DST' not found"
    fi
    exit 1
fi

# Check distrib
if [ -e "/etc/fedora-release" ]; then
    version=$(cat /etc/fedora-release | cut -d " " -f3)
    echo "Fedora $version found"
    if [ "$version" -gt 20 ] ; then
        install="dnf"
    else
        install="yum"
    fi

    PACKAGES="libusb-devel openssl-devel gcc"

    $install list installed $PACKAGES 1>/dev/null
    if [ "$?" != "0" ] ; then
        $install install -q -y $PACKAGES 1>/dev/null
    fi

else
    version=$(grep -i -o -P "Ubuntu \d+" /etc/lsb-release | cut -d " " -f2)
    if [ "$version" = "" ]; then
        echo "Sorry, installation procedure only available on Fedora/Ubuntu distributions."
        exit 1
    fi

    echo "Ubuntu v$version found"
    apt-get -qq install -y libssl-dev libusb-1.0-0-dev 1>/dev/null
fi



# Compil
make -s -C ${DIR}
if [ "$?" = "0" ] ; then
    strip ${DIR}/trx_lms7002m.so
    rm -f ${DST}/trx_lms7002m.so
    ln -s ${DIR}/trx_lms7002m.so ${DST}/trx_lms7002m.so
else
    echo "Error while compiling trx driver"
fi

# Delete default files and copy configs
rm -Rf ${DST}/config/lms-sodera
rm -Rf ${DST}/config/lms-stream-unite7
cp -r ${DIR}/config-limeSDR/ ${DST}/config/limeSDR
${DST}/config/rf_select.sh limeSDR

exit 0

