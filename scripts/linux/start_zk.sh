#!/bin/bash
#
# Options:
#    INSTALL_DIR    <dir>
#    PORT           <port>

PROJECT_DIR=$(realpath $(dirname $(dirname $(dirname "${BASH_SOURCE[0]}"))))

if [ -z "$INSTALL_DIR" ]
then
    echo "ERROR: no INSTALL_DIR specified"
    exit 1
fi

if [ -z "$PORT" ]
then
    echo "ERROR: no PORT specified"
    exit 1
fi

mkdir -p $INSTALL_DIR
if [ $? -ne 0 ]
then
    echo "ERROR: mkdir $PREFIX failed"
    exit 1
fi

cd $INSTALL_DIR

ZOOKEEPER_ROOT=apache-zookeeper-3.7.0-bin
ZOOKEEPER_TAR_NAME=${ZOOKEEPER_ROOT}.tar.gz
ZOOKEEPER_TAR_MD5_VALUE="8ffa97e7e6b0b2cf1d022e5156a7561a"

if [ ! -f $ZOOKEEPER_TAR_NAME ]; then
    echo "Downloading zookeeper..."
    download_url="http://pegasus-thirdparty-package.oss-cn-beijing.aliyuncs.com/apache-zookeeper-3.7.0-bin.tar.gz"
    if ! wget -T 5 -t 1 $download_url; then
        echo "ERROR: download zookeeper failed"
        exit 1
    fi
    if [ `md5sum $ZOOKEEPER_TAR_NAME | awk '{print$1}'` != $ZOOKEEPER_TAR_MD5_VALUE ]; then
        echo "check file $ZOOKEEPER_TAR_NAME md5sum failed!"
        exit 1
    fi
fi

if [ ! -d $ZOOKEEPER_ROOT ]; then
    echo "Decompressing zookeeper..."
    if ! tar xf $ZOOKEEPER_TAR_NAME; then
        echo "ERROR: decompress zookeeper failed"
        exit 1
    fi
fi

ZOOKEEPER_HOME=`pwd`/$ZOOKEEPER_ROOT
ZOOKEEPER_PORT=$PORT

cp $ZOOKEEPER_HOME/conf/zoo_sample.cfg $ZOOKEEPER_HOME/conf/zoo.cfg
sed -i "s@dataDir=/tmp/zookeeper@dataDir=$ZOOKEEPER_HOME/data@" $ZOOKEEPER_HOME/conf/zoo.cfg
sed -i "s@clientPort=2181@clientPort=$ZOOKEEPER_PORT@" $ZOOKEEPER_HOME/conf/zoo.cfg
echo "admin.enableServer=false" >> $ZOOKEEPER_HOME/conf/zoo.cfg
echo "4lw.commands.whitelist=ruok" >> $ZOOKEEPER_HOME/conf/zoo.cfg

mkdir -p $ZOOKEEPER_HOME/data
$ZOOKEEPER_HOME/bin/zkServer.sh start
sleep 1

zk_check_count=0
while true; do
    sleep 1 # wait until zookeeper bootstrapped
    if echo ruok | nc localhost "$ZOOKEEPER_PORT" | grep -q imok; then
        echo "Zookeeper started at port $ZOOKEEPER_PORT"
        exit 0
    fi
    zk_check_count=$((zk_check_count+1))
    echo "ERROR: starting zookeeper has failed ${zk_check_count} times"
    if [ $zk_check_count -gt 30 ]; then
        echo "ERROR: failed to start zookeeper in 30 seconds"
        exit 1
    fi
done
