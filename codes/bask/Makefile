#OFED_KERNEL := /usr/src/ofa_kernel/default

# KBUILD_EXTRA_SYMBOLS := /usr/src/ofa_kernel/default/Module.symvers
#KBUILD_EXTRA_SYMBOLS := $(OFED_KERNEL)/Module.symvers
# KBUILD_EXTRA_ += -I$(OFED_KERNEL)/include
GLIB_FLAGS := $(shell pkg-config --cflags --libs glib-2.0)

KDIR=/lib/modules/`uname -r`/build

obj-m += client_bridge.o

client_bridge-y		:= client_stub.o rdma_stub.o 

all: do_rsync build

build:
	$(MAKE) -C $(KDIR) M=$(PWD) modules
	mv client_bridge.ko bask_client_bridge.ko
	$(MAKE) -C $(KDIR) M=$(PWD) EXTRA_CFLAGS="-DKSM_OFFLOAD_MODE=NO_OFFLOAD" modules
	mv client_bridge.ko no_client_bridge.ko
	$(MAKE) -C $(KDIR) M=$(PWD) EXTRA_CFLAGS="-DKSM_OFFLOAD_MODE=SINGLE_OPERATION_OFFLOAD" modules
	mv client_bridge.ko dataplane_client_bridge.ko
	gcc -g -O0 -o bask_server server.c -lrdmacm -libverbs -lxxhash $(GLIB_FLAGS)

sanitize:
	$(MAKE) -C $(KDIR) M=$(PWD) modules
	gcc -g -O0 -fsanitize=address -o bask_server server.c -lrdmacm -libverbs -lxxhash $(GLIB_FLAGS)

fast:
	$(MAKE) -C $(KDIR) M=$(PWD) modules
	gcc -g -O3 -o bask_server server.c -lrdmacm -libverbs -lxxhash $(GLIB_FLAGS)

do_rsync: clean
	rsync --progress --exclude '.git' --exclude '.cache' * ubuntu@192.168.100.2:~/bask_snic/

clean:
	cp compile_commands.json backup
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f bask_server
	rm -f *_client.birdge.ko
	mv backup compile_commands.json
