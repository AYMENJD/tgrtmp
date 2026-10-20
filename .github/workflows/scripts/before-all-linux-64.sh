#!/usr/bin/env bash
set -e

git clone --depth 1 --branch OpenSSL_1_1_1 https://github.com/openssl/openssl
cd openssl
./config
make -j$(($(nproc) + 2))
make install
ldconfig
cd .. && rm -rf openssl
