/*
 * Copyright (c) 2015 Intel Corporation.
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
 */

#include <errno.h>

#include <device.h>
#include <drivers/ioapic.h>
#include <init.h>
#include <nanokernel.h>
#include <rtc.h>
#include <power.h>

#include "qm_isr.h"
#include "qm_rtc.h"

struct rtc_data {
#ifdef CONFIG_RTC_QMSI_API_REENTRANCY
	struct nano_sem sem;
#endif
#ifdef CONFIG_DEVICE_POWER_MANAGEMENT
	uint32_t device_power_state;
#endif
};

#define RTC_HAS_CONTEXT_DATA \
	(CONFIG_RTC_QMSI_API_REENTRANCY || CONFIG_DEVICE_POWER_MANAGEMENT)

#if RTC_HAS_CONTEXT_DATA
static struct rtc_data rtc_context;
#define RTC_CONTEXT (&rtc_context)
#else
#define RTC_CONTEXT (NULL)
#endif /* RTC_HAS_CONTEXT_DATA */

#ifdef CONFIG_RTC_QMSI_API_REENTRANCY
static const int reentrancy_protection = 1;
#define RP_GET(dev) (&((struct rtc_data *)(dev->driver_data))->sem)
#else
static const int reentrancy_protection;
#define RP_GET(dev) (NULL)
#endif

#ifdef CONFIG_DEVICE_POWER_MANAGEMENT

static void rtc_qmsi_set_power_state(struct device *dev, uint32_t power_state)
{
	struct rtc_data *context = dev->driver_data;

	context->device_power_state = power_state;
}

static uint32_t rtc_qmsi_get_power_state(struct device *dev)
{
	struct rtc_data *context = dev->driver_data;

	return context->device_power_state;
}
#else
#define rtc_qmsi_set_power_state(...)
#endif

static void rtc_reentrancy_init(struct device *dev)
{
	if (!reentrancy_protection) {
		return;
	}

	nano_sem_init(RP_GET(dev));
	nano_sem_give(RP_GET(dev));
}

static void rtc_critical_region_start(struct device *dev)
{
	if (!reentrancy_protection) {
		return;
	}

	nano_sem_take(RP_GET(dev), TICKS_UNLIMITED);
}

static void rtc_critical_region_end(struct device *dev)
{
	if (!reentrancy_protection) {
		return;
	}

	nano_sem_give(RP_GET(dev));
}

static void rtc_qmsi_enable(struct device *dev)
{
	clk_periph_enable(CLK_PERIPH_RTC_REGISTER | CLK_PERIPH_CLK);
}

static void rtc_qmsi_disable(struct device *dev)
{
	clk_periph_disable(CLK_PERIPH_RTC_REGISTER);
}


static int rtc_qmsi_set_config(struct device *dev, struct rtc_config *cfg)
{
	qm_rtc_config_t qm_cfg;
	int result = 0;

	qm_cfg.init_val = cfg->init_val;
	qm_cfg.alarm_en = cfg->alarm_enable;
	qm_cfg.alarm_val = cfg->alarm_val;
	/* Casting callback type due different input parameter from QMSI
	 * compared aganst the Zephyr callback from void cb(struct device *dev)
	 * to void cb(void *)
	 */
	qm_cfg.callback = (void *) cfg->cb_fn;
	qm_cfg.callback_data = dev;

	/* Set prescaler value. Ideally, the divider should come from struct
	 * rtc_config instead. It's safe to use RTC_DIVIDER here for now since
	 * values defined by clk_rtc_div and by QMSI's clk_rtc_div_t match for
	 * both D2000 and SE.
	 */
	qm_cfg.prescaler = RTC_DIVIDER;

	rtc_critical_region_start(dev);

	if (qm_rtc_set_config(QM_RTC_0, &qm_cfg)) {
		result = -EIO;
	}

	rtc_critical_region_end(dev);

	return result;
}

static int rtc_qmsi_set_alarm(struct device *dev, const uint32_t alarm_val)
{
	return qm_rtc_set_alarm(QM_RTC_0, alarm_val);
}

static uint32_t rtc_qmsi_read(struct device *dev)
{
	return QM_RTC[QM_RTC_0].rtc_ccvr;
}

static const struct rtc_driver_api api = {
	.enable = rtc_qmsi_enable,
	.disable = rtc_qmsi_disable,
	.read = rtc_qmsi_read,
	.set_config = rtc_qmsi_set_config,
	.set_alarm = rtc_qmsi_set_alarm,
};

static int rtc_qmsi_init(struct device *dev)
{
	rtc_reentrancy_init(dev);

	IRQ_CONNECT(QM_IRQ_RTC_0_INT, CONFIG_RTC_0_IRQ_PRI,
		    qm_rtc_0_isr, NULL,
		    IOAPIC_EDGE | IOAPIC_HIGH);

	/* Unmask RTC interrupt */
	irq_enable(QM_IRQ_RTC_0_INT);

	/* Route RTC interrupt to Lakemont */
	QM_INTERRUPT_ROUTER->rtc_0_int_mask &= ~BIT(0);

	rtc_qmsi_set_power_state(dev, DEVICE_PM_ACTIVE_STATE);

	return 0;
}

#ifdef CONFIG_DEVICE_POWER_MANAGEMENT
static uint32_t int_rtc_mask_save;

static int rtc_suspend_device(struct device *dev)
{
	int_rtc_mask_save = QM_INTERRUPT_ROUTER->rtc_0_int_mask;
	rtc_qmsi_set_power_state(dev, DEVICE_PM_SUSPEND_STATE);
	return 0;
}

static int rtc_resume_device(struct device *dev)
{
	QM_INTERRUPT_ROUTER->rtc_0_int_mask = int_rtc_mask_save;
	rtc_qmsi_set_power_state(dev, DEVICE_PM_ACTIVE_STATE);
	return 0;
}

/*
* Implements the driver control management functionality
* the *context may include IN data or/and OUT data
*/
static int rtc_qmsi_device_ctrl(struct device *dev, uint32_t ctrl_command,
				void *context)
{
	if (ctrl_command == DEVICE_PM_SET_POWER_STATE) {
		if (*((uint32_t *)context) == DEVICE_PM_SUSPEND_STATE) {
			return rtc_suspend_device(dev);
		} else if (*((uint32_t *)context) == DEVICE_PM_ACTIVE_STATE) {
			return rtc_resume_device(dev);
		}
	} else if (ctrl_command == DEVICE_PM_GET_POWER_STATE) {
		*((uint32_t *)context) = rtc_qmsi_get_power_state(dev);
		return 0;
	}

	return 0;
}
#endif

DEVICE_DEFINE(rtc, CONFIG_RTC_0_NAME, &rtc_qmsi_init, rtc_qmsi_device_ctrl,
	      RTC_CONTEXT, NULL, SECONDARY, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,
	      &api);
