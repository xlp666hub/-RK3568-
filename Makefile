KERNEL_DIR := /home/xlp/workspace/kernel

obj-m := alarm_key_drv.o alarm_io_drv.o bh1750_drv.o mpu6050_drv.o

all:
	make -C $(KERNEL_DIR) M=$(PWD) ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- LOCALVERSION=-test1 modules

clean:
	make -C $(KERNEL_DIR) M=$(PWD) clean
