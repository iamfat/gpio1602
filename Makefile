DRIVER_VERSION  := 1.0.0.000

KDIR    := /lib/modules/$(shell uname -r)/build 
PWD := $(shell pwd)
OBJ := gpio1602

obj-m   := $(OBJ).o

EXTRA_CFLAGS    := -DDRIVER_VERSION=\"v$(DRIVER_VERSION)\"

all:    clean compile

compile:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

load:   
	su -c "insmod ./usbdpfp.ko"

load_debug: 
	@echo "try \"tail -f /var/log/message\" in another window as root...";
	su -c "insmod ./$(OBJ).ko debug=1"
 
unload:
	-su -c "rmmod -s $(OBJ)"

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean

debug: unload clean compile load_debug
