ccflags-y +=-I$(srctree)/kernel/nanokernel/include
ccflags-y +=-I$(srctree)/kernel/microkernel/include


asflags-y := ${ccflags-y}

obj-y = nano_fiber.o nano_lifo.o \
	nano_fifo.o nano_stack.o nano_sys_clock.o \
	nano_context.o nano_init.o nano_sema.o \
	version.o device.o wait_q.o

obj-$(CONFIG_INT_LATENCY_BENCHMARK) += int_latency_bench.o
obj-$(CONFIG_NANO_TIMEOUTS) += nano_sleep.o
obj-$(CONFIG_STACK_CANARIES) += compiler_stack_protect.o
obj-$(CONFIG_SYS_POWER_MANAGEMENT) += idle.o
obj-$(CONFIG_NANO_TIMERS) += nano_timer.o
obj-$(CONFIG_KERNEL_EVENT_LOGGER) += event_logger.o
obj-$(CONFIG_KERNEL_EVENT_LOGGER) += kernel_event_logger.o
obj-$(CONFIG_ATOMIC_OPERATIONS_C) += atomic_c.o
obj-$(CONFIG_ERRNO) += errno.o
obj-$(CONFIG_NANO_WORKQUEUE) += nano_work.o
ifneq (,$(filter y,$(CONFIG_NANO_TIMERS) $(CONFIG_NANO_TIMEOUTS)))
obj-y += timeout_q.o
endif
