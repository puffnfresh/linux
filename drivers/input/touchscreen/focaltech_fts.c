// SPDX-License-Identifier: GPL-2.0-only
/*
 *
 * FocalTech touchscreen driver.
 *
 * Copyright (c) 2010-2017, FocalTech Systems, Ltd., all rights reserved.
 * Copyright (C) 2018 XiaoMi, Inc.
 * Copyright (c) 2021 Caleb Connolly <caleb@connolly.tech>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/of_device.h>
#include <linux/regulator/consumer.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/time.h>
#include <linux/workqueue.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/types.h>
#include <linux/device.h>
#include <linux/netdevice.h>
#include <linux/unistd.h>
#include <linux/vmalloc.h>
#include <linux/notifier.h>

#define FTS_CMD_START1 0x55
#define FTS_CMD_START2 0xAA
#define FTS_CMD_START_DELAY 10
#define FTS_CMD_READ_ID 0x90
#define FTS_CMD_READ_ID_LEN 4

#define FTS_REG_INT_CNT 0x8F
#define FTS_REG_FLOW_WORK_CNT 0x91
#define FTS_REG_WORKMODE 0x00
#define FTS_REG_WORKMODE_FACTORY_VALUE 0x40
#define FTS_REG_WORKMODE_WORK_VALUE 0x00
#define FTS_REG_ESDCHECK_DISABLE 0x8D
#define FTS_REG_CHIP_ID 0xA3
#define FTS_REG_CHIP_ID2 0x9F
#define FTS_REG_POWER_MODE 0xA5
#define FTS_REG_POWER_MODE_SLEEP_VALUE 0x03
#define FTS_REG_FW_VER 0xA6
#define FTS_REG_VENDOR_ID 0xA8
#define FTS_REG_LCD_BUSY_NUM 0xAB
#define FTS_REG_FACE_DEC_MODE_EN 0xB0
#define FTS_REG_FACE_DEC_MODE_STATUS 0x01
#define FTS_REG_IDE_PARA_VER_ID 0xB5
#define FTS_REG_IDE_PARA_STATUS 0xB6
#define FTS_REG_GLOVE_MODE_EN 0xC0
#define FTS_REG_COVER_MODE_EN 0xC1
#define FTS_REG_CHARGER_MODE_EN 0x8B
#define FTS_REG_GESTURE_EN 0xD0
#define FTS_REG_GESTURE_OUTPUT_ADDRESS 0xD3
#define FTS_REG_MODULE_ID 0xE3
#define FTS_REG_LIC_VER 0xE4
#define FTS_REG_ESD_SATURATE 0xED

#define FTS_MAX_POINTS_SUPPORT 10
#define FTS_ONE_TCH_LEN 6

#define FTS_MAX_ID 0x0A
#define FTS_TOUCH_X_H_POS 3
#define FTS_TOUCH_X_L_POS 4
#define FTS_TOUCH_Y_H_POS 5
#define FTS_TOUCH_Y_L_POS 6
#define FTS_TOUCH_PRE_POS 7
#define FTS_TOUCH_AREA_POS 8
#define FTS_TOUCH_POINT_NUM 2
#define FTS_TOUCH_EVENT_POS 3
#define FTS_TOUCH_ID_POS 5
#define FTS_COORDS_ARR_SIZE 2

#define FTS_TOUCH_DOWN 0
#define FTS_TOUCH_UP 1
#define FTS_TOUCH_CONTACT 2

#define EVENT_DOWN(flag) ((flag == FTS_TOUCH_DOWN) || (flag == FTS_TOUCH_CONTACT))

#define FTS_LOCKDOWN_INFO_SIZE 8
#define LOCKDOWN_INFO_ADDR 0x1FA0

#define FTS_DRIVER_NAME "fts-i2c"
#define INTERVAL_READ_REG 100 /* unit:ms */
#define TIMEOUT_READ_REG 2000 /* unit:ms */
#define FTS_VDD_MIN_UV 2600000
#define FTS_VDD_MAX_UV 3300000
#define FTS_I2C_VCC_MIN_UV 1800000
#define FTS_I2C_VCC_MAX_UV 1800000

#define I2C_RETRY_NUMBER 3

#define CHIP_TYPE_5452 0x5452
#define CHIP_TYPE_8719 0x8719

static DEFINE_MUTEX(i2c_rw_access);

struct ts_event {
	int x; /*x coordinate */
	int y; /*y coordinate */
	int p; /* pressure */
	int flag; /* touch event flag: 0 -- down; 1-- up; 2 -- contact */
	int id; /*touch ID */
	int area;
};

struct fts_ts_data {
	struct i2c_client *client;
	struct input_dev *input_dev;
	struct regulator *vdd;
	struct regulator *vcc_i2c;
	spinlock_t irq_lock;
	struct mutex report_mutex;
	int irq;
	bool irq_disabled;
	bool power_disabled;

	/* multi-touch */
	struct ts_event *events;
	u32 max_touch_number;
	u8 *point_buf;
	int pnt_buf_size;
	int touches;
	int touch_point;
	int point_num;
	bool dev_pm_suspend;
	bool low_power_mode;
	struct completion dev_pm_suspend_completion;
	struct pinctrl *pinctrl;

	// DT data
	struct gpio_desc *irq_gpio;
	struct gpio_desc *reset_gpio;
	u32 width;
	u32 height;
};

int fts_i2c_read(struct i2c_client *client, char *writebuf, int writelen,
		 char *readbuf, int readlen)
{
	int ret = 0;
	int msg_count = !!writelen + 1;
	struct i2c_msg msgs[2];

	if (readlen < 0 || writelen < 0)
		return -EINVAL;

	// If writelen is zero then only populate msgs[0].
	// otherwise we read into msgs[1]
	msgs[msg_count-1].len = readlen;
	msgs[msg_count-1].buf = readbuf;
	msgs[msg_count-1].addr = client->addr;
	msgs[msg_count-1].flags = I2C_M_RD;

	if (writelen > 0) {
		msgs[0].len = writelen;
		msgs[0].buf = writebuf;
		msgs[0].addr = client->addr;
		msgs[0].flags = 0;
	}

	mutex_lock(&i2c_rw_access);

	ret = i2c_transfer(client->adapter, msgs, msg_count);

	mutex_unlock(&i2c_rw_access);
	return ret;
}

int fts_i2c_read_reg(struct i2c_client *client, u8 regaddr, u8 *regvalue)
{
	return fts_i2c_read(client, &regaddr, 1, regvalue, 1);
}

static bool fts_chip_is_valid(struct fts_ts_data *data, u16 id)
{
	if (id != CHIP_TYPE_5452 && id != CHIP_TYPE_8719)
		return false;

	return true;
}

int fts_wait_ready(struct fts_ts_data *data)
{
	int ret = 0;
	int cnt = 0;
	u8 reg_value[2];
	struct i2c_client *client = data->client;

	do {
		ret = fts_i2c_read_reg(client, FTS_REG_CHIP_ID, &reg_value[0]);
		ret = fts_i2c_read_reg(client, FTS_REG_CHIP_ID2, &reg_value[1]);
		if (fts_chip_is_valid(data, reg_value[0] << 8 | reg_value[1])) {
			dev_dbg(&data->client->dev, "TP Ready, Device ID = 0x%x%x, count = %d",
				reg_value[0], reg_value[1], cnt);
			return 0;
		}
		cnt++;
		msleep(INTERVAL_READ_REG);
	} while ((cnt * INTERVAL_READ_REG) < TIMEOUT_READ_REG);

	return -EIO;
}

static int fts_power_source_init(struct fts_ts_data *data)
{
	int ret = 0;

	data->vdd = devm_regulator_get(&data->client->dev, "vdd");
	if (IS_ERR_OR_NULL(data->vdd)) {
		ret = PTR_ERR(data->vdd);
		dev_err(&data->client->dev, "get vdd regulator failed,ret=%d", ret);
		return ret;
	}

	if (regulator_count_voltages(data->vdd) > 0) {
		ret = regulator_set_voltage(data->vdd, FTS_VDD_MIN_UV,
					    FTS_VDD_MAX_UV);
		if (ret < 0) {
			dev_err(&data->client->dev, "failed to set vdd regulator ret=%d", ret);
			goto exit;
		}
	}

	data->vcc_i2c = devm_regulator_get(&data->client->dev, "vcc-i2c");
	if (IS_ERR(data->vcc_i2c)) {
		ret = PTR_ERR(data->vcc_i2c);
		dev_err(&data->client->dev, "get vcc_i2c regulator failed,ret=%d", ret);
		return ret;
	}

	if (regulator_count_voltages(data->vcc_i2c) > 0) {
		ret = regulator_set_voltage(data->vcc_i2c, FTS_I2C_VCC_MIN_UV,
					    FTS_I2C_VCC_MAX_UV);
		if (ret < 0) {
			dev_err(&data->client->dev, "failed to set vcc_i2c regulator ret=%d",
				ret);
			goto exit;
		}
	}

exit:
	return ret;
}

static int fts_power_source_release(struct fts_ts_data *data)
{
	if (!data->power_disabled) {
		regulator_disable(data->vdd);
		regulator_disable(data->vcc_i2c);
	}

	devm_regulator_put(data->vdd);
	devm_regulator_put(data->vcc_i2c);

	return 0;
}

static int fts_power_source_ctrl(struct fts_ts_data *data, bool enable)
{
	int ret = 0;

	if (enable) {
		if (data->power_disabled) {
			ret = regulator_enable(data->vdd);
			if (ret < 0) {
				dev_err(&data->client->dev,
					"enable vdd regulator failed,ret=%d",
					ret);
			}

			ret = regulator_enable(data->vcc_i2c);
			if (ret < 0) {
				dev_err(&data->client->dev, "enable vcc_i2c regulator failed,ret=%d",
					  ret);
			}
			data->power_disabled = false;
		}
	} else {
		if (!data->power_disabled) {
			ret = regulator_disable(data->vdd);
			if (ret < 0) {
				dev_err(&data->client->dev,
					"disable vdd regulator failed,ret=%d",
					ret);
			}

			ret = regulator_disable(data->vcc_i2c);
			if (ret < 0) {
				dev_err(&data->client->dev, "disable vcc_i2c regulator failed,ret=%d",
					  ret);
			}

			data->power_disabled = true;
		}
	}

	return ret;
}

static int fts_pinctrl_set_active(struct fts_ts_data *data, bool enable)
{
	int ret = 0;
	struct pinctrl_state *state = pinctrl_lookup_state(data->pinctrl,
		enable ? "default" : "suspend");

	if (IS_ERR_OR_NULL(state)) {
		dev_err(&data->client->dev, "pinctrl lookup %s failed\n",
			enable ? "default" : "suspend");
		return -EINVAL;
	}

	ret = pinctrl_select_state(data->pinctrl, state);
	if (ret < 0) {
		dev_err(&data->client->dev,
			"Failed to set pinctrl state: enable = %d, ret = %d",
			enable, ret);
	}

	return ret;
}

static void fts_release_all_finger(struct fts_ts_data *data)
{
	struct input_dev *input_dev = data->input_dev;
	u32 finger_count = 0;

	mutex_lock(&data->report_mutex);

	for (finger_count = 0; finger_count < data->max_touch_number;
	     finger_count++) {
		input_mt_slot(input_dev, finger_count);
		input_mt_report_slot_state(input_dev, MT_TOOL_FINGER, false);
	}

	input_report_key(input_dev, BTN_TOUCH, 0);
	input_sync(input_dev);

	mutex_unlock(&data->report_mutex);
}

static int fts_input_report_b(struct fts_ts_data *data)
{
	int i = 0;
	int uppoint = 0;
	int touches = 0;
	bool va_reported = false;
	struct ts_event *events = data->events;

	for (i = 0; i < data->touch_point; i++) {
		if (events[i].id >= data->max_touch_number)
			break;

		va_reported = true;
		input_mt_slot(data->input_dev, events[i].id);

		if (events[i].flag == FTS_TOUCH_DOWN || events[i].flag == FTS_TOUCH_CONTACT) {
			input_mt_report_slot_state(data->input_dev,
						   MT_TOOL_FINGER, true);

			if (events[i].p <= 0)
				events[i].p = 0x3f;

			input_report_abs(data->input_dev, ABS_MT_PRESSURE,
					 events[i].p);

			if (events[i].area <= 0)
				events[i].area = 0x09;

			input_report_abs(data->input_dev, ABS_MT_TOUCH_MAJOR,
					 events[i].area);
			input_report_abs(data->input_dev, ABS_MT_POSITION_X,
					 events[i].x);
			input_report_abs(data->input_dev, ABS_MT_POSITION_Y,
					 events[i].y);

			touches |= BIT(events[i].id);
			data->touches |= BIT(events[i].id);
		} else {
			uppoint++;

			input_report_abs(data->input_dev, ABS_MT_PRESSURE, 0);

			input_mt_report_slot_state(data->input_dev,
						   MT_TOOL_FINGER, false);
			data->touches &= ~BIT(events[i].id);
		}
	}

	if (data->touches ^ touches) {
		for (i = 0; i < data->max_touch_number; i++) {
			if (BIT(i) & (data->touches ^ touches)) {
				va_reported = true;
				input_mt_slot(data->input_dev, i);
				input_mt_report_slot_state(
					data->input_dev, MT_TOOL_FINGER, false);
			}
		}
	}
	data->touches = touches;

	if (va_reported) {
		if (!data->point_num || !touches)
			input_report_key(data->input_dev, BTN_TOUCH, 0);
		else
			input_report_key(data->input_dev, BTN_TOUCH, 1);
	} else {
		dev_err(&data->client->dev, "va not reported, but touches=%d", touches);
	}

	input_sync(data->input_dev);
	return 0;
}

static int fts_read_touchdata(struct fts_ts_data *data)
{
	int ret = 0;
	int i = 0;
	u8 pointid;
	int base;
	struct ts_event *events = data->events;
	int max_touch_num = data->max_touch_number;
	u8 *buf = data->point_buf;

	data->point_num = 0;
	data->touch_point = 0;

	memset(buf, 0xFF, data->pnt_buf_size);
	buf[0] = 0x00;

	ret = fts_i2c_read(data->client, buf, 1, buf, data->pnt_buf_size);
	if (ret < 0) {
		dev_err(&data->client->dev, "read touchdata failed, ret:%d", ret);
		return ret;
	}
	data->point_num = buf[FTS_TOUCH_POINT_NUM] & 0x0F;

	if (data->point_num > max_touch_num)
		return -EINVAL;

	for (i = 0; i < max_touch_num; i++) {
		base = FTS_ONE_TCH_LEN * i;

		pointid = (buf[FTS_TOUCH_ID_POS + base]) >> 4;
		if (pointid >= FTS_MAX_ID)
			break;

		else if (pointid >= max_touch_num)
			return -EINVAL;

		data->touch_point++;

		events[i].x = ((buf[FTS_TOUCH_X_H_POS + base] & 0x0F) << 8) +
			      (buf[FTS_TOUCH_X_L_POS + base] & 0xFF);
		events[i].y = ((buf[FTS_TOUCH_Y_H_POS + base] & 0x0F) << 8) +
			      (buf[FTS_TOUCH_Y_L_POS + base] & 0xFF);
		events[i].flag = buf[FTS_TOUCH_EVENT_POS + base] >> 6;
		events[i].id = buf[FTS_TOUCH_ID_POS + base] >> 4;
		events[i].area = buf[FTS_TOUCH_AREA_POS + base] >> 4;
		events[i].p = buf[FTS_TOUCH_PRE_POS + base];

		if (((events[i].flag == FTS_TOUCH_DOWN) || (events[i].flag == FTS_TOUCH_CONTACT))
		    && (data->point_num == 0)) {
			dev_err(&data->client->dev, "abnormal touch data from fw");
			return -EIO;
		}
	}
	if (data->touch_point == 0) {
		dev_err(&data->client->dev, "no touch point information");
		return -EIO;
	}

	return 0;
}

static void fts_report_event(struct fts_ts_data *data)
{
	fts_input_report_b(data);
}

static irqreturn_t fts_ts_interrupt(int irq, void *d)
{
	int ret = 0;
	struct fts_ts_data *data = (struct fts_ts_data *)d;

	if (!data) {
		dev_err(&data->client->dev, "%s() Invalid fts_ts_data", __func__);
		return IRQ_HANDLED;
	}

	if (data->dev_pm_suspend) {
		ret = wait_for_completion_timeout(
			&data->dev_pm_suspend_completion,
			msecs_to_jiffies(700));
		if (!ret) {
			dev_err(&data->client->dev,
				"Didn't resume in time, skipping wakeup event handling\n");
			return IRQ_HANDLED;
		}
	}

	ret = fts_read_touchdata(data);
	if (ret == 0) {
		mutex_lock(&data->report_mutex);
		fts_report_event(data);
		mutex_unlock(&data->report_mutex);
	}

	return IRQ_HANDLED;
}

static int fts_input_init(struct fts_ts_data *data)
{
	int ret = 0;
	struct input_dev *input_dev;

	input_dev = input_allocate_device();
	if (!input_dev) {
		dev_err(&data->client->dev, "Failed to allocate memory for input device");
		return -ENOMEM;
	}

	/* Init and register Input device */
	input_dev->name = FTS_DRIVER_NAME;
	input_dev->id.bustype = BUS_I2C;
	input_dev->dev.parent = &data->client->dev;

	input_set_drvdata(input_dev, data);

	__set_bit(EV_SYN, input_dev->evbit);
	__set_bit(EV_ABS, input_dev->evbit);
	__set_bit(BTN_TOUCH, input_dev->keybit);
	__set_bit(INPUT_PROP_DIRECT, input_dev->propbit);

	input_mt_init_slots(input_dev, data->max_touch_number,
			    INPUT_MT_DIRECT);

	input_set_abs_params(input_dev, ABS_MT_POSITION_X, 0,
			     data->width - 1, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_POSITION_Y, 0,
			     data->height - 1, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_TOUCH_MAJOR, 0, 0xFF, 0, 0);

	input_set_abs_params(input_dev, ABS_MT_PRESSURE, 0, 0xFF, 0, 0);

	data->pnt_buf_size = data->max_touch_number * FTS_ONE_TCH_LEN + 3;
	data->point_buf = devm_kzalloc(&data->client->dev, data->pnt_buf_size, GFP_KERNEL);
	if (!data->point_buf) {
		dev_err(&data->client->dev, "Failed to alloc memory for point buf!");
		ret = -ENOMEM;
		goto err_out;
	}

	data->events = devm_kzalloc(&data->client->dev,
		data->max_touch_number * sizeof(struct ts_event), GFP_KERNEL);
	if (!data->events) {
		ret = -ENOMEM;
		goto err_out;
	}
	ret = input_register_device(input_dev);
	if (ret < 0) {
		dev_err(&data->client->dev, "Input device registration failed");
		goto err_out;
	}

	data->input_dev = input_dev;

	return 0;

err_out:
	input_set_drvdata(input_dev, NULL);
	input_free_device(input_dev);
	input_dev = NULL;

	return ret;
}

static int fts_reset(struct fts_ts_data *data)
{
	gpiod_set_value_cansleep(data->reset_gpio, 0);
	msleep(20);
	gpiod_set_value_cansleep(data->reset_gpio, 1);

	return 0;
}

static int fts_parse_dt(struct fts_ts_data *data)
{
	int ret = 0;
	struct device *dev = &data->client->dev;
	struct device_node *np = dev->of_node;
	u32 val;

	ret = of_property_read_u32(np, "touchscreen-size-x", &data->width);
	if (ret < 0) {
		dev_err(&data->client->dev, "Unable to read property 'touchscreen-size-x'");
		return -EINVAL;
	}

	ret = of_property_read_u32(np, "touchscreen-size-y", &data->height);
	if (ret < 0) {
		dev_err(&data->client->dev, "Unable to read property 'touchscreen-size-y'");
		return -EINVAL;
	}

	ret = of_property_read_u32(np, "focaltech,max-touch-number", &val);
	if (ret < 0) {
		dev_err(&data->client->dev, "Unable to read property 'focaltech,max-touch-number'");
		return -EINVAL;
	}
	if (val < 2 || val > FTS_MAX_POINTS_SUPPORT) {
		dev_err(&data->client->dev, "'focaltech,max-touch-number' out of range [2, %d]",
			FTS_MAX_POINTS_SUPPORT);
		return -EINVAL;
	}
	data->max_touch_number = val;

	data->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (data->reset_gpio < 0) {
		dev_err(&data->client->dev, "Unable to get reset gpio");
		return -EINVAL;
	}

	data->irq_gpio = devm_gpiod_get_optional(dev, "irq", GPIOD_OUT_LOW);

	return 0;
}

static int fts_ts_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	int ret = 0;
	struct fts_ts_data *data;
	struct pinctrl_state *pinctrl_state_temp;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(&data->client->dev, "I2C not supported");
		return -ENODEV;
	}

	data = devm_kzalloc(&client->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->client = client;
	ret = fts_parse_dt(data);
	if (ret < 0)
		return ret;

	i2c_set_clientdata(client, data);

	spin_lock_init(&data->irq_lock);
	mutex_init(&data->report_mutex);

	ret = fts_input_init(data);
	if (ret < 0) {
		dev_err(&data->client->dev, "Input initialization fail");
		goto err_input_init;
	}

	ret = fts_power_source_init(data);
	if (ret < 0) {
		dev_err(&data->client->dev, "fail to get vdd/vcc_i2c regulator");
		goto err_power_init;
	}

	data->power_disabled = true;
	ret = fts_power_source_ctrl(data, true);
	if (ret < 0) {
		dev_err(&data->client->dev, "fail to enable vdd/vcc_i2c regulator");
		goto err_power_ctrl;
	}

	data->pinctrl = devm_pinctrl_get(&client->dev);
	if (IS_ERR_OR_NULL(data->pinctrl)) {
		dev_err(&data->client->dev, "Failed to get pinctrl, please check dts");
		ret = PTR_ERR(data->pinctrl);
		goto err_power_ctrl;
	}

	pinctrl_state_temp = pinctrl_lookup_state(data->pinctrl, "default");
	if (IS_ERR_OR_NULL(pinctrl_state_temp) ||
		IS_ERR_OR_NULL(pinctrl_lookup_state(data->pinctrl, "suspend"))) {
		dev_err(&data->client->dev, "Failed to get default or suspend pinctrl state, please check dts");
		goto err_power_ctrl;
	}

	fts_pinctrl_set_active(data, true);

	ret = fts_reset(data);
	if (ret < 0) {
		dev_err(&data->client->dev, "Failed to reset chip");
		goto err_gpio_config;
	}

	ret = fts_wait_ready(data);
	if (ret < 0) {
		dev_err(&data->client->dev, "Touch IC didn't turn on or is unsupported");
		goto err_gpio_config;
	}

	if (data->irq_gpio) {
		data->irq = gpiod_to_irq(data->irq_gpio);

		ret = request_threaded_irq(data->irq, NULL, fts_ts_interrupt,
					IRQF_ONESHOT,
					data->client->name, data);
		if (ret < 0) {
			dev_err(&data->client->dev, "request irq failed");
			goto err_gpio_config;
		}
		device_init_wakeup(&client->dev, true);
	}

	data->dev_pm_suspend = false;
	init_completion(&data->dev_pm_suspend_completion);

	return 0;

err_gpio_config:
	fts_power_source_ctrl(data, false);
err_power_ctrl:
	fts_power_source_release(data);
err_power_init:
	input_unregister_device(data->input_dev);
err_input_init:
	devm_kfree(&client->dev, data);

	return ret;
}

static int fts_ts_remove(struct i2c_client *client)
{
	struct fts_ts_data *data = i2c_get_clientdata(client);

	free_irq(client->irq, data);
	input_unregister_device(data->input_dev);

	fts_power_source_ctrl(data, false);
	fts_power_source_release(data);

	kfree(data->point_buf);
	kfree(data->events);

	devm_kfree(&client->dev, data);

	return 0;
}

static int fts_ts_suspend(struct device *dev)
{
	struct fts_ts_data *data = dev_get_drvdata(dev);
	int ret = 0;

	disable_irq(data->irq);

	ret = fts_power_source_ctrl(data, false);
	if (ret < 0)
		dev_err(dev, "power off fail, ret=%d", ret);
	fts_pinctrl_set_active(data, false);

	return 0;
}

static int fts_ts_resume(struct device *dev)
{
	struct fts_ts_data *data = dev_get_drvdata(dev);

	fts_release_all_finger(data);

	fts_power_source_ctrl(data, true);
	fts_pinctrl_set_active(data, true);

	fts_wait_ready(data);

	enable_irq(data->irq);

	return 0;
}

static int fts_pm_suspend(struct device *dev)
{
	struct fts_ts_data *data = dev_get_drvdata(dev);
	int ret = 0;

	data->dev_pm_suspend = true;

	if (data->low_power_mode) {
		ret = enable_irq_wake(data->irq);
		if (ret < 0) {
			dev_err(&data->client->dev, "enable_irq_wake(irq:%d) failed",
				 data->irq);
		}
	} else {
		ret = fts_ts_suspend(dev);
	}

	reinit_completion(&data->dev_pm_suspend_completion);

	return ret;
}

static int fts_pm_resume(struct device *dev)
{
	struct fts_ts_data *data = dev_get_drvdata(dev);
	int ret = 0;

	data->dev_pm_suspend = false;

	if (data->low_power_mode) {
		ret = disable_irq_wake(data->irq);
		if (ret < 0) {
			dev_err(&data->client->dev, "disable_irq_wake(irq:%d) failed",
				 data->irq);
		}
	} else {
		ret = fts_ts_resume(dev);
	}

	complete(&data->dev_pm_suspend_completion);

	return 0;
}

static const struct dev_pm_ops fts_dev_pm_ops = {
	.suspend = fts_pm_suspend,
	.resume = fts_pm_resume,
};

static const struct of_device_id fts_match_table[] = {
	{ .compatible = "focaltech,fts5452", },
	{ .compatible = "focaltech,fts8719", },
	{ /* sentinel */ },
};

MODULE_DEVICE_TABLE(of, fts_match_table);

static struct i2c_driver fts_ts_driver = {
	.probe = fts_ts_probe,
	.remove = fts_ts_remove,
	.driver = {
		.name = FTS_DRIVER_NAME,
		.pm = &fts_dev_pm_ops,
		.of_match_table = fts_match_table,
	},
};
module_i2c_driver(fts_ts_driver);

MODULE_AUTHOR("Caleb Connolly <caleb@connolly.tech>");
MODULE_DESCRIPTION("FocalTech touchscreen Driver");
MODULE_LICENSE("GPL v2");
