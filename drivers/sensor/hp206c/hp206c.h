/* HopeRF Electronic HP206C precision barometer and altimeter header
 *
 * Copyright (c) 2016 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#ifndef _SENSOR_HP206C_H_
#define _SENSOR_HP206C_H_

#include <misc/util.h>

#define HP206C_I2C_ADDRESS				0x76

/* HP206C configuration registers */
#define HP206C_REG_ALT_OFF_LSB				0x00
#define HP206C_REG_ALT_OFF_MSB				0x01
#define HP206C_REG_PA_H_TH_LSB				0x02
#define HP206C_REG_PA_H_TH_MSB				0x03
#define HP206C_REG_PA_M_TH_LSB				0x04
#define HP206C_REG_PA_M_TH_MSB				0x05
#define HP206C_REG_PA_L_TH_LSB				0x06
#define HP206C_REG_PA_L_TH_MSB				0x07
#define HP206C_REG_T_H_TH				0x08
#define HP206C_REG_T_M_TH				0x09
#define HP206C_REG_T_L_TH				0x0A
#define HP206C_REG_INT_EN				0x0B
#define HP206C_REG_INT_GFG				0x0C
#define HP206C_REG_INT_SRC				0x0D
#define HP206C_REG_INT_DIR				0x0E
#define HP206C_REG_PARA					0x0F

/* HP206C commands */
#define HP206C_CMD_SOFT_RST				0x06
#define HP206C_CMD_ADC_CVT				0x40
#define HP206C_CMD_READ_PT				0x10
#define HP206C_CMD_READ_AT				0x11
#define HP206C_CMD_READ_P				0x30
#define HP206C_CMD_READ_A				0x31
#define HP206C_CMD_READ_T				0x32
#define HP206C_CMD_ANA_CAL				0x28
#define HP206C_CMD_READ_REG				0x80
#define HP206C_CMD_WRITE_REG				0xC0

#define HP206C_REG_ADDR_MASK				0x3F

/* HP206C_REG_INT_SRC */
#define HP206C_T_WIN					BIT(0)
#define HP206C_PA_WIN					BIT(1)
#define HP206C_T_TRAV					BIT(2)
#define HP206C_PA_TRAV					BIT(3)
#define HP206C_T_RDY					BIT(4)
#define HP206C_PA_RDY					BIT(5)
#define HP206C_DEV_RDY					BIT(6)
#define HP206C_TH_ERR					BIT(7)

/* HP206C_REG_PARA */
#define HP206C_COMPENSATION_EN				BIT(7)

/* default settings, based on menuconfig options */
#if defined(CONFIG_HP206C_OSR_RUNTIME)
#	define HP206C_DEFAULT_OSR			4096
#else
#	define HP206C_DEFAULT_OSR			CONFIG_HP206C_OSR
#endif

#if defined(CONFIG_HP206C_ALT_OFFSET_RUNTIME)
#	define HP206C_DEFAULT_ALT_OFFSET		0
#else
#	define HP206C_DEFAULT_ALT_OFFSET		CONFIG_HP206C_ALT_OFFSET
#endif
/* end of default settings */

struct hp206c_device_data {
	struct device *i2c;
#ifdef CONFIG_NANO_TIMERS
#if CONFIG_SYS_CLOCK_TICKS_PER_SEC < 2000
#error "SYS_CLOCK_TICKS_PER_SEC >= 2000 needed for better timeouts granularity."
#endif
	struct nano_timer tmr;
#endif
	uint8_t osr;
};

#define SYS_LOG_DOMAIN "HP206C"
#define SYS_LOG_LEVEL CONFIG_SYS_LOG_SENSOR_LEVEL
#include <misc/sys_log.h>
#endif /* _SENSOR_HP206C_H_ */
