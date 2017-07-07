obj-m := wakeup_timer.o

KERNEL_SRC ?= /home/eichenberger/projects/nbhw16/linux-stable/

PWD := $(shell pwd)

all:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) modules

modules_install:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) modules_install

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) clean

test: test.c
	$(CC) test.c -o test_ids_control

.PHONY: all modules_install clean test 
