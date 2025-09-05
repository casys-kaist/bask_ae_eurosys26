set -eu -o pipefail

# Modified from Memstrata script

sed -i '/^case $- in$/,/^esac$/d' ~/.bashrc
sed -i '1i force_color_prompt=""' ~/.bashrc
sed -i '1i color_prompt=""' ~/.bashrc

sudo apt update
sudo apt install make gcc g++

# Gapbs
cd ~
git clone https://github.com/sbeamer/gapbs.git
cd gapbs
make bfs
# Requires large memory
make benchmark/graphs/web.sg

# faster
cd ~
sudo apt update
sudo apt install libtbb-dev -y
sudo apt install libaio-dev libaio1 uuid-dev libnuma-dev cmake -y

git clone https://github.com/yuhong-zhong/FASTER.git
cd FASTER/cc
mkdir -p build/Release
cd build/Release
cmake -DCMAKE_BUILD_TYPE=Release ../..
make pmem_benchmark

# spark
cd ~
sudo apt update
sudo apt install openjdk-8-jre-headless openjdk-8-jdk-headless -y
echo 'export JAVA_HOME=/usr/lib/jvm/java-8-openjdk-amd64' >> ~/.bashrc
source ~/.bashrc
sudo bash -c "echo 'export JAVA_HOME=/usr/lib/jvm/java-8-openjdk-amd64' >> /etc/environment"
source /etc/environment

cd ~
sudo apt-get install ssh pdsh -y
wget https://dlcdn.apache.org/hadoop/common/hadoop-3.2.4/hadoop-3.2.4.tar.gz
tar -xzvf hadoop-3.2.4.tar.gz
mv hadoop-3.2.4 hadoop

cd hadoop
sed -i "s|<configuration>||" etc/hadoop/core-site.xml
sed -i "s|</configuration>||" etc/hadoop/core-site.xml
echo "<configuration>" >> etc/hadoop/core-site.xml
echo "<property>" >> etc/hadoop/core-site.xml
echo "<name>fs.defaultFS</name>" >> etc/hadoop/core-site.xml
echo "<value>hdfs://localhost:8020</value>" >> etc/hadoop/core-site.xml
echo "</property>" >> etc/hadoop/core-site.xml
echo "</configuration>" >> etc/hadoop/core-site.xml

sed -i "s|<configuration>||" etc/hadoop/hdfs-site.xml
sed -i "s|</configuration>||" etc/hadoop/hdfs-site.xml
cat >> etc/hadoop/hdfs-site.xml <<- End
<configuration>
    <property>
        <name>dfs.replication</name>
        <value>1</value>
    </property>
    <property>
        <name>dfs.datanode.data.dir</name>
        <value>/home/ubuntu/hdfs/datanode</value>
    </property>
</configuration>
End

mkdir -p /home/ubuntu/hdfs/datanode
chmod -R 777 /home/ubuntu/hdfs/datanode

ssh-keygen -t rsa -P '' -f ~/.ssh/id_rsa
cat ~/.ssh/id_rsa.pub >> ~/.ssh/authorized_keys
chmod 0600 ~/.ssh/authorized_keys

# Formate the filesystem
bin/hdfs namenode -format

echo 'export PDSH_RCMD_TYPE=ssh' >> ~/.bashrc
source ~/.bashrc

sed -i "s|<configuration>||" etc/hadoop/mapred-site.xml
sed -i "s|</configuration>||" etc/hadoop/mapred-site.xml
HADOOP_MAPRED_HOME='$HADOOP_MAPRED_HOME'
cat >> etc/hadoop/mapred-site.xml <<- End
<configuration>
    <property>
        <name>mapreduce.framework.name</name>
        <value>yarn</value>
    </property>
    <property>
        <name>yarn.app.mapreduce.am.env</name>
        <value>HADOOP_MAPRED_HOME=/home/ubuntu/hadoop</value>
    </property>
    <property>
        <name>mapreduce.map.env</name>
        <value>HADOOP_MAPRED_HOME=/home/ubuntu/hadoop</value>
    </property>
    <property>
        <name>mapreduce.reduce.env</name>
        <value>HADOOP_MAPRED_HOME=/home/ubuntu/hadoop</value>
    </property>
    <property>
        <name>mapreduce.application.classpath</name>
        <value>$HADOOP_MAPRED_HOME/share/hadoop/mapreduce/*,$HADOOP_MAPRED_HOME/share/hadoop/mapreduce/lib/*,$HADOOP_MAPRED_HOME/share/hadoop/common/*,$HADOOP_MAPRED_HOME/share/hadoop/common/lib/*,$HADOOP_MAPRED_HOME/share/hadoop/yarn/*,$HADOOP_MAPRED_HOME/share/hadoop/yarn/lib/*,$HADOOP_MAPRED_HOME/share/hadoop/hdfs/*,$HADOOP_MAPRED_HOME/share/hadoop/hdfs/lib/*</value>
    </property>
</configuration>
End

sed -i "s|<configuration>||" etc/hadoop/yarn-site.xml
sed -i "s|</configuration>||" etc/hadoop/yarn-site.xml
cat >> etc/hadoop/yarn-site.xml <<- End
<configuration>
    <property>
        <name>yarn.nodemanager.aux-services</name>
        <value>mapreduce_shuffle</value>
    </property>
    <property>
        <name>yarn.nodemanager.env-whitelist</name>
        <value>JAVA_HOME,HADOOP_COMMON_HOME,HADOOP_HDFS_HOME,HADOOP_CONF_DIR,CLASSPATH_PREPEND_DISTCACHE,HADOOP_YARN_HOME,HADOOP_HOME,PATH,LANG,TZ,HADOOP_MAPRED_HOME</value>
    </property>
</configuration>
End

cd ~
wget https://archive.apache.org/dist/spark/spark-2.4.0/spark-2.4.0-bin-hadoop2.7.tgz
tar -xzvf spark-2.4.0-bin-hadoop2.7.tgz
mv spark-2.4.0-bin-hadoop2.7 spark
cd spark
echo "export SPARK_HOME=$(pwd)" >> ~/.bashrc
echo 'export PATH=$PATH:$SPARK_HOME/bin' >> ~/.bashrc
source ~/.bashrc

cd ~
#wget https://mirrors.estointernet.in/apache/maven/maven-3/3.6.3/binaries/apache-maven-3.6.3-bin.tar.gz
wget https://repo.maven.apache.org/maven2/org/apache/maven/apache-maven/3.6.3/apache-maven-3.6.3-bin.tar.gz
tar -xzvf apache-maven-3.6.3-bin.tar.gz
mv apache-maven-3.6.3 apache-maven
cd apache-maven
echo "export M2_HOME=$(pwd)" >> ~/.bashrc
echo 'export PATH=$PATH:$M2_HOME/bin' >> ~/.bashrc
source ~/.bashrc

cd ~
sudo apt-get update
sudo apt-get install bc scala python2 maven -y

wget https://github.com/Intel-bigdata/HiBench/archive/refs/tags/v7.1.1.tar.gz
tar -xzvf v7.1.1.tar.gz
mv HiBench-7.1.1 HiBench
cd HiBench

cp hadoopbench/mahout/pom.xml hadoopbench/mahout/pom.xml.bak
cat hadoopbench/mahout/pom.xml \
    | sed 's|<repo2>http://archive.cloudera.com</repo2>|<repo2>https://archive.apache.org</repo2>|' \
    | sed 's|cdh5/cdh/5/mahout-0.9-cdh5.1.0.tar.gz|dist/mahout/0.9/mahout-distribution-0.9.tar.gz|' \
    | sed 's|aa953e0353ac104a22d314d15c88d78f|09b999fbee70c9853789ffbd8f28b8a3|' \
    > ./pom.xml.tmp
mv ./pom.xml.tmp hadoopbench/mahout/pom.xml

mvn -Phadoopbench -Psparkbench -Dspark=2.4 -Dscala=2.11 clean package
 #here
cp conf/hadoop.conf.template conf/hadoop.conf
sed -i "s|^hibench.hadoop.home.*|hibench.hadoop.home /home/ubuntu/hadoop|" conf/hadoop.conf
echo "hibench.hadoop.examples.jar /home/ubuntu/hadoop/share/hadoop/mapreduce/hadoop-mapreduce-examples-3.2.4.jar" >> conf/hadoop.conf

cp conf/spark.conf.template conf/spark.conf
sed -i "s|hibench.spark.home.*|hibench.spark.home /home/ubuntu/spark|" conf/spark.conf
sed -i "s|hibench.yarn.executor.num.*|hibench.yarn.executor.num 2|" conf/spark.conf
sed -i "s|hibench.yarn.executor.cores.*|hibench.yarn.executor.cores 2|" conf/spark.conf
sed -i "s|spark.executor.memory.*|spark.executor.memory 4g|" conf/spark.conf
sed -i "s|spark.driver.memory.*|spark.driver.memory 4g|" conf/spark.conf

echo "hibench.masters.hostnames localhost" >> conf/spark.conf
echo "hibench.slaves.hostnames localhost" >> conf/spark.conf

sed -i "s|hibench.scale.profile.*|hibench.scale.profile large|" conf/hibench.conf


# DLRM
cd ~
sudo apt-get install linux-tools-common linux-tools-generic linux-tools-`uname -r` -y
BASE_DIRECTORY_NAME="dlrm"

rm -rf $BASE_DIRECTORY_NAME
mkdir -p $BASE_DIRECTORY_NAME
cd $BASE_DIRECTORY_NAME
export BASE_PATH=$(pwd)
echo "DLRM-SETUP: FINISHED SETTING UP BASE DIRECTORY"

echo BASE_PATH=$BASE_PATH >> $BASE_PATH/paths.export

wget https://apt.repos.intel.com/intel-gpg-keys/GPG-PUB-KEY-INTEL-SW-PRODUCTS.PUB
sudo apt-key add GPG-PUB-KEY-INTEL-SW-PRODUCTS.PUB
echo "deb https://apt.repos.intel.com/oneapi all main" | \
        sudo tee /etc/apt/sources.list.d/oneAPI.list
sudo apt update
sudo apt-get install pkg-config
sudo apt -y install cmake intel-oneapi-vtune numactl python3-pip
sudo sed -i '1i DIAGUTIL_PATH=""' /opt/intel/oneapi/vtune/latest/env/vars.sh
source /opt/intel/oneapi/vtune/latest/env/vars.sh
echo "DLRM-SETUP: FINISHED INSTALLING VTUNE"

cd $BASE_PATH
mkdir -p miniconda3
wget https://repo.anaconda.com/miniconda/Miniconda3-latest-Linux-x86_64.sh \
        -O miniconda3/miniconda.sh
/usr/bin/bash miniconda3/miniconda.sh -b -u -p miniconda3
rm -rf miniconda3/miniconda.sh
miniconda3/bin/conda tos accept --override-channels --channel https://repo.anaconda.com/pkgs/main
miniconda3/bin/conda tos accept --override-channels --channel https://repo.anaconda.com/pkgs/r
miniconda3/bin/conda init zsh
miniconda3/bin/conda init bash
miniconda3/bin/conda create --name dlrm_cpu python=3.9 ipython -y
echo "DLRM-SETUP: FINISHED INSTALLING CONDA"
source ~/.bashrc

conda install -n dlrm_cpu astunparse cffi cmake dataclasses future mkl mkl-include ninja \
        pyyaml requests setuptools six typing_extensions -y
conda install -n dlrm_cpu -c conda-forge jemalloc gcc=12.1.0 -y
conda run -n dlrm_cpu pip install git+https://github.com/mlperf/logging
conda run -n dlrm_cpu pip install onnx lark-parser hypothesis tqdm scikit-learn
echo "DLRM-SETUP: FINISHED SETTING UP CONDA ENV"

cd $BASE_PATH
git clone --recursive -b v1.12.1 https://github.com/pytorch/pytorch
cd pytorch
conda run --no-capture-output -n dlrm_cpu pip install -r requirements.txt
conda run --no-capture-output -n dlrm_cpu pip install numpy==1.26.4
export CMAKE_PREFIX_PATH=${CONDA_PREFIX:-"$(dirname $(which conda))/../"}
echo CMAKE_PREFIX_PATH=$CMAKE_PREFIX_PATH >> $BASE_PATH/paths.export
export TORCH_PATH=$(pwd)
echo TORCH_PATH=$TORCH_PATH >> $BASE_PATH/paths.export
conda run --no-capture-output -n dlrm_cpu python setup.py develop
echo "DLRM-SETUP: FINISHED BUILDLING PYTORCH"

cd $BASE_PATH
git clone --recursive -b v1.12.300 https://github.com/intel/intel-extension-for-pytorch
cd intel-extension-for-pytorch
export IPEX_PATH=$(pwd)
echo IPEX_PATH=$IPEX_PATH >> $BASE_PATH/paths.export
echo "DLRM-SETUP: FINISHED CLONING IPEX"

cd $BASE_PATH
git clone https://github.com/NERSC/itt-python
cd itt-python
git checkout 3fb76911c81cc9ae5ee55101080a58461b99e11c
export VTUNE_PROFILER_DIR=/opt/intel/oneapi/vtune/latest
echo VTUNE_PROFILER_DIR=$VTUNE_PROFILER_DIR >> $BASE_PATH/paths.export
conda run --no-capture-output -n dlrm_cpu python setup.py install --vtune=$VTUNE_PROFILER_DIR
echo "DLRM-SETUP: FINISHED BUILDLING ITT-PYTHON"

# Set up DLRM inference test.
cd $BASE_PATH
git clone https://github.com/rishucoding/reproduce_isca23_cpu_DLRM_inference
cd reproduce_isca23_cpu_DLRM_inference
export DLRM_SYSTEM=$(pwd)
echo DLRM_SYSTEM=$DLRM_SYSTEM >> $BASE_PATH/paths.export
export MODELS_PATH=$(pwd)
echo MODELS_PATH=$MODELS_PATH >> $BASE_PATH/paths.export
mkdir -p models/recommendation/pytorch/dlrm/
cd models/recommendation/pytorch/dlrm
git clone https://github.com/facebookresearch/dlrm.git product

cp $DLRM_SYSTEM/dlrm_patches/dlrm_data_pytorch.py \
    product/dlrm_data_pytorch.py
cp $DLRM_SYSTEM/dlrm_patches/dlrm_s_pytorch.py \
    product/dlrm_s_pytorch.py
echo "DLRM-SETUP: FINISHED SETTING UP DLRM TEST"

cd $IPEX_PATH
git apply $DLRM_SYSTEM/dlrm_patches/ipex.patch
find . -type f -exec sed -i 's/-Werror//g' {} \;
conda install -n dlrm_cpu cmake=3.22
USE_NATIVE_ARCH=1 CXXFLAGS="-D_GLIBCXX_USE_CXX11_ABI=0" conda run --no-capture-output -n dlrm_cpu python setup.py install
echo "DLRM-SETUP: FINISHED BUILDING IPEX"

# redis
cd ~
wget https://github.com/redis/redis/archive/refs/tags/7.2.3.tar.gz
tar -xzvf 7.2.3.tar.gz
mv redis-7.2.3 redis
cd redis
make -j8
sudo bash -c "echo 'vm.overcommit_memory=1' >> /etc/sysctl.conf"
sed -i -e '$a save ""' redis.conf

# memcached
cd ~
sudo apt-get update
sudo apt-get install memcached -y

# ycsb
cd ~
git clone https://github.com/brianfrankcooper/YCSB.git
cd YCSB
mvn -pl site.ycsb:redis-binding -am clean package
mvn -pl site.ycsb:memcached-binding -am clean package
