#!/bin/sh
set -x

if [[ "x$TRAVIS_TAG" == "x" ]] ; then
    cmake -DCMAKE_BUILD_TYPE=Debug $@
else
    RPATH_ARG=""
    DEPLOYMENT_TARGET_ARG=""
    if [[ $TRAVIS_OS_NAME == "linux" ]] ; then
        RPATH_ARG="-DCMAKE_INSTALL_RPATH=\\\$ORIGIN/../lib"
    else
        RPATH_ARG="-DENABLE_INSTALL_RPATH=OFF"
        DEPLOYMENT_TARGET_ARG="-DCMAKE_OSX_DEPLOYMENT_TARGET=10.10"
    fi
    cmake -DCMAKE_BUILD_TYPE=Release -DBUNDLE_QT=ON "$RPATH_ARG" "$DEPLOYMENT_TARGET_ARG" $@
fi

