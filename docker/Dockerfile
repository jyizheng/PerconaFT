FROM debian:buster

MAINTAINER Yizheng Jiao <yizheng@cs.unc.edu>

ARG DEBIAN_FRONTEND=noninteractive
ENV LC_ALL=en_US.UTF-8 LANG=en_US.UTF-8 LC_CTYPE=en_US.UTF-8 IS_DOCKER_ENV=true container=docker
ENV PATH "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

RUN useradd -u 1000 -d /home/tiger -m -s /bin/bash tiger && mkdir -p /home/tiger/.service/ && mkdir -p /opt/tiger && mkdir -p /var/log/tiger && mkdir -p /opt/log/tiger && chown -R tiger:tiger /home/tiger && chown -R tiger:tiger /opt/tiger && chown -R tiger:tiger /var/log/tiger && chown -R tiger:tiger /opt/log/tiger
RUN apt-get update && apt-get install gnupg2 autoconf autoconf-archive automake libtool m4 uuid-dev libnuma-dev -y --yes

RUN export DEBIAN_FRONTEND=noninteractive \
    && apt-get update \
    && apt-get install -y krb5-user xz-utils wget git vim sudo gdb libzstd-dev curl libssl-dev zlib1g-dev build-essential lsof python3 python3-distutils \
    && apt-get clean && curl https://bootstrap.pypa.io/get-pip.py --output get-pip.py \
    && python3 get-pip.py && pip install requests \
    && pip install meson pyelftools ninja \
    && wget -q https://cmake.org/files/v3.20/cmake-3.20.4.tar.gz -O /tmp/cmake-3.20.4.tar.gz \
    && mkdir -p /tmp/cmake \
    && tar -zxf /tmp/cmake-3.20.4.tar.gz -C /tmp/cmake --strip-components=1 \
    && cd /tmp/cmake \
    && mkdir build && cd build && ../bootstrap \
    && make -j4 \
    && make install \
    && cd /tmp \
    && git clone https://github.com/linux-test-project/lcov.git \
    && cd lcov \
    && git checkout v1.15 \
    && make install \
    && git clone https://github.com/eriwen/lcov-to-cobertura-xml.git \
    && cp lcov-to-cobertura-xml/lcov_cobertura/lcov_cobertura.py / \
    && cd /tmp \
    && git clone https://github.com/westerndigitalcorporation/libzbd.git \
    && cd libzbd \
    && sh ./autogen.sh \
    && ./configure \
    && make && make install \
    && rm -rf /tmp/* && rm -rf /var/lib/apt/lists/* /tmp/* /var/tmp/*

RUN apt-get update && apt-get install fuse libfuse-dev -y --yes && apt-get clean
RUN apt-get clean && rm -rf /var/lib/apt/lists/* /tmp/* /var/tmp/*

