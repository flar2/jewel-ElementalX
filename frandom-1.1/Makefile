# Makefile for 2.6 kernels
KERNEL_BUILD := /home/aaron/HTC_EVO_4G_LTE/ElementalX-EVO
KERNEL_CROSS_COMPILE := arm-linux-gnueabi-

obj-m	:= frandom.o

all:
	make -C $(KERNEL_BUILD) CROSS_COMPILE=$(KERNEL_CROSS_COMPILE) M=$(PWD) modules
	$(KERNEL_CROSS_COMPILE)strip --strip-debug frandom.ko

clean:
	make -C $(KERNEL_BUILD) M=$(PWD) clean 2> /dev/null
	rm -f modules.order *~
