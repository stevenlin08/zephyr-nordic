ccflags-$(CONFIG_PINMUX_DEV) +=-I$(srctree)/drivers
ccflags-$(CONFIG_PINMUX_DEV_K64) +=-I$(srctree)/drivers/pinmux/k64/
ccflags-$(CONFIG_PINMUX_DEV_QMSI) += -I$(CONFIG_QMSI_INSTALL_PATH)/include

obj-$(CONFIG_PINMUX_DEV_ATMEL_SAM3X) += pinmux_dev_atmel_sam3x.o
obj-$(CONFIG_PINMUX_DEV_K64) += pinmux_dev_k64.o
obj-$(CONFIG_PINMUX_DEV_QMSI) += pinmux_dev_qmsi.o
obj-$(CONFIG_PINMUX_DEV_STM32) += pinmux_dev_stm32.o
