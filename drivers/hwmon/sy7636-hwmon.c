/*
 * Copyright (C) 2010 Freescale Semiconductor, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 */
/*
 * sy7636-hwmon.c
 *
 * Copyright (C) 2003-2004 Alexey Fisher <fishor@mail.ru>
 *                         Jean Delvare <khali@linux-fr.org>
 *
 * The SY7636 is a sensor chip made by Silergy .
 * It reports up to two temperatures (its own plus up to
 * one external one).
 */


#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/sysfs.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/mfd/sy7636.h>

#include <linux/gpio.h>

/*
 * Client data (each client gets its own)
 */
struct sy7636_data {
	struct device *hwmon_dev;
	struct sy7636 *sy7636;
};



int sy7636_get_temperature(struct sy7636 *sy7636,int *O_piTemperature)
{
	int iTemp = 0;
	unsigned int reg_val;
	int iChk;

	if(!sy7636) {
		printk(KERN_ERR"%s(),SY7636 object error !!\n",__FUNCTION__);
		return -1;
	}

	iChk = SY7636_REG_READ(sy7636,THERM);
	if(iChk>=0) {
		reg_val = iChk;
	}
	else {
		printk(KERN_ERR"%s(),SY7636 temperature read error !!\n",__FUNCTION__);
		return -1;
	}
	iTemp = reg_val;

	if(O_piTemperature) {
		printk("%s():temperature = %d,reg=0x%x\n",__FUNCTION__,iTemp,reg_val);
		*O_piTemperature = iTemp;
	}
	
	return 0;
}
EXPORT_SYMBOL(sy7636_get_temperature);

int sy7636_set_vcom(struct sy7636 *sy7636,int iVCOMmV,int iIsWriteToFlash)
{
	//long vcom_reg_val ;
	int iRet = 0;
	unsigned int regVCOM1=0, regVCOM2=0;
	unsigned short wVCOM_val ;
	int iChk;
	//printk("%s(%d);\n",__FUNCTION__,__LINE__);

	if(!sy7636) {
		printk(KERN_ERR"%s(),SY7636 object error !!\n",__FUNCTION__);
		return -1;
	}

	if(iVCOMmV>0) {
		printk(KERN_ERR"%s(),VCOMmV cannot <=0 !!\n",__FUNCTION__);
		return -2;
	}


	wVCOM_val = (unsigned short)((iVCOMmV)/10);
	dev_info(sy7636->dev, "vcom=>%dmV,wVCOM_val=0x%x\n",
			iVCOMmV,wVCOM_val);

	/*
	 * get the interrupt status register value
	 */
	do
	{

		iChk = SY7636_REG_READ(sy7636,VCOM_ADJ2);
		if(iChk>=0) {
			regVCOM2 = iChk;
		}
		else {
			dev_err(sy7636->dev,"SY7636 VCOM2 reading error !\n");
			iRet = -4;
			break;
		}

		regVCOM1 = (unsigned char)(wVCOM_val&0xff);
		iChk = SY7636_REG_WRITE_EX(sy7636,VCOM_ADJ1,regVCOM1);
		if(iChk<0) {
			dev_err(sy7636->dev, "write regVCOM1=0x%x failed\n",regVCOM1);
			iRet = -5;
		}

		if(wVCOM_val&0x100) {
			regVCOM2 |= 0x80;
		}
		else {
			regVCOM2 &= ~0x80;
		}

		iChk = SY7636_REG_WRITE_EX(sy7636,VCOM_ADJ2,regVCOM2);
		if(iChk<0) {
			dev_err(sy7636->dev, "write regVCOM2=0x%x failed\n",regVCOM2);
			iRet = -5;
		}

		dev_info(sy7636->dev, "write regVCOM1=0x%x,regVCOM2=0x%x\n",regVCOM1,regVCOM2);

		if(iRet>=0) {
			sy7636->vcom_uV = iVCOMmV*1000;
		}


	}while(0);

	//printk("%s(%d);\n",__FUNCTION__,__LINE__);
	return iRet;
}
EXPORT_SYMBOL(sy7636_set_vcom);

int sy7636_get_vcom(struct sy7636 *sy7636,int *O_piVCOMmV)
{
	unsigned int reg_val;
	unsigned int vcom_reg_val;
	unsigned short wTemp;
	int iVCOMmV;
	int iChk;

	//printk("%s(%d),sy7636=%p;\n",__FUNCTION__,__LINE__,sy7636);

	if(!sy7636) {
		return -1;
	}

	if(sy7636->need_reinit) {
		iVCOMmV = sy7636->vcom_uV/1000;
		if(O_piVCOMmV) {
			*O_piVCOMmV = iVCOMmV;
			printk("return cached VCOM=%dmV\n",*O_piVCOMmV);
		}
		else {
			printk(KERN_ERR"%s():parameter error !!\n",__FUNCTION__);
		}
		sy7636_set_vcom(sy7636,iVCOMmV,0);
		return 0;
	}

	/*
	 * get the vcom registers
	 */

	iChk = SY7636_REG_READ(sy7636,VCOM_ADJ1);
	if(iChk>=0) {
		vcom_reg_val = iChk;
	}
	else {
		return -1;
	}
	iChk = SY7636_REG_READ(sy7636,VCOM_ADJ2);
	if(iChk>=0) {
		reg_val = iChk;
	}
	else {
		return -1;
	}
	wTemp = vcom_reg_val;
	if(reg_val&0x80) {
		wTemp |= 0x100;
	}
	else {
		wTemp &= ~0x100;
	}
	iVCOMmV = -(wTemp*10);

	if(O_piVCOMmV) {
		*O_piVCOMmV = iVCOMmV;
	}
	
	//printk("%s(%d);\n",__FUNCTION__,__LINE__);
	return 0;
}
EXPORT_SYMBOL(sy7636_get_vcom);

static int sy7636_read(struct device *dev, enum hwmon_sensor_types type,
                         u32 attr, int channel, long *temp)
{
	struct regmap *regmap = dev_get_drvdata(dev);
	int ret, reg_val;

	if (attr != hwmon_temp_input)
		return -EOPNOTSUPP;

	ret = regmap_read(regmap, REG_SY7636_THERM, &reg_val);

	if (ret)
		return ret;

	*temp = reg_val;

	return 0;
}

static umode_t sy7636_is_visible(const void *data,
                                   enum hwmon_sensor_types type,
                                   u32 attr, int channel)
{
	if (type != hwmon_temp)
		return 0;

	if (attr != hwmon_temp_input)
		return 0;

	return 0444;
}

static const struct hwmon_ops sy7636_hwmon_ops = {
	.is_visible = sy7636_is_visible,
	.read = sy7636_read,
};

static const struct hwmon_channel_info *sy7636_info[] = {
	HWMON_CHANNEL_INFO(chip, HWMON_C_REGISTER_TZ),
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT),
	NULL
};

static const struct hwmon_chip_info sy7636_chip_info = {
	.ops = &sy7636_hwmon_ops,
	.info = sy7636_info,
};

static int sy7636_sensor_probe(struct platform_device *pdev)
{
	struct regmap *regmap = dev_get_regmap(pdev->dev.parent, NULL);
	struct device *hwmon_dev;
	int err;

	if (!regmap)
			return -EPROBE_DEFER;

	pdev->dev.of_node = pdev->dev.parent->of_node;
	hwmon_dev = devm_hwmon_device_register_with_info(&pdev->dev,
														"sy7636_temperature", regmap,
														&sy7636_chip_info, NULL);

	if (IS_ERR(hwmon_dev)) {
			err = PTR_ERR(hwmon_dev);
			dev_err(&pdev->dev, "Unable to register hwmon device, returned %d\n", err);
			return err;
	}

	return 0;
}

static const struct platform_device_id sy7636_sns_id[] = {
	{ "sy7636-sns", 0},
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(platform, sy7636_sns_id);

/*
 * Driver data (common to all clients)
 */
static struct platform_driver sy7636_sensor_driver = {
	.probe = sy7636_sensor_probe,
	.id_table = sy7636_sns_id,
	.driver = {
		.name = "sy7636_sensor",
	},
};

module_platform_driver(sy7636_sensor_driver);

MODULE_DESCRIPTION("SY7636 sensor driver");
MODULE_LICENSE("GPL");
