#
# Copyright (c) 2016 Intel Corporation
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

# This Makefile is like a replacement of the $(ZEPHYR_BASE)/Makefile
# for libraries. So, we need to define ARCH, and do it before including
# Makefile.toolchain.*
ARCH ?= x86

# This include will set CROSS_COMPILE & TOOLCHAIN_CFLAGS for us. It uses
# two-pass compile flag configuration, so needs to be included twice.
include $(ZEPHYR_BASE)/scripts/Makefile.toolchain.$(ZEPHYR_GCC_VARIANT)
include $(ZEPHYR_BASE)/scripts/Makefile.toolchain.$(ZEPHYR_GCC_VARIANT)

CC = $(CROSS_COMPILE)gcc
AR = $(CROSS_COMPILE)ar

all:
	mkdir -p obj lib
	$(CC) -c $(TOOLCHAIN_CFLAGS) -Iinclude src/mylib.c -o obj/mylib.o
	$(AR) -rcs lib/libmylib.a obj/mylib.o

clean:
	rm -rf obj lib

