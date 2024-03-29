#!/bin/bash

# 创建编译的依赖文件夹
mkdir -p /tmp/tmp_dir
mkdir -p /home/tiger/.ssh/
mkdir -p /opt/tiger

# 覆盖 kerberos 的配置到公司的配置
cp -f /build/krb5.conf /etc/krb5.conf

# 创建用于编译的git配置的链接，文件会在编译的时候从外部挂到容器内
ln -s /home/tiger/.ssh/gitconfig /home/tiger/.gitconfig

mkdir -p /opt/common_tools
rm -rf /bd_build/*
chown -R tiger:tiger /home/tiger
chown -R tiger:tiger /opt/tiger
chmod 700 -R /home/tiger/.ssh
chmod 777 -R /tmp