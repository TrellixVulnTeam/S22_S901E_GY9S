// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/debugfs.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/regmap.h>
#include <linux/qti-regmap-debugfs.h>
#include <linux/regulator/consumer.h>
#include <linux/types.h>
#include <linux/usb/repeater.h>
#if IS_ENABLED(CONFIG_USB_PHY_TUNING_QCOM)
#include <linux/sec_class.h>
#include <linux/mutex.h>
#endif

#define EUSB2_3P0_VOL_MIN			3075000 /* uV */
#define EUSB2_3P0_VOL_MAX			3300000 /* uV */
#define EUSB2_3P0_HPM_LOAD			3500	/* uA */

#define EUSB2_1P8_VOL_MIN			1800000 /* uV */
#define EUSB2_1P8_VOL_MAX			1800000 /* uV */
#define EUSB2_1P8_HPM_LOAD			32000	/* uA */

/* NXP PTN3222 eUSB2 repeater registers */
#define RESET_CONTROL			0x01
#define LINK_CONTROL1			0x02
#define LINK_CONTROL2			0x03
#define eUSB2_RX_CONTROL		0x04
#define eUSB2_TX_CONTROL		0x05
#define USB2_RX_CONTROL			0x06
#define USB2_TX_CONTROL1		0x07
#define USB2_TX_CONTROL2		0x08
#define USB2_HS_TERMINATION		0x09
#define USB2_HS_DISCONNECT_THRESHOLD	0x0A
#define RAP_SIGNATURE			0x0D
#define DEVICE_STATUS			0x0F
#define LINK_STATUS			0x10
#define REVISION_ID			0x13
#define CHIP_ID_0			0x14
#define CHIP_ID_1			0x15
#define CHIP_ID_2			0x16

/* TI eUSB2 repeater registers */
#define GPIO0_CONFIG			0x00
#define GPIO1_CONFIG			0x40
#define UART_PORT1			0x50
#define EXTRA_PORT1			0x51
#define REV_ID				0xB0
#define GLOBAL_CONFIG			0xB2
#define INT_ENABLE_1			0xB3
#define INT_ENABLE_2			0xB4
#define BC_CONTROL			0xB6
#define BC_STATUS_1			0xB7
#define INT_STATUS_1			0xA3
#define INT_STATUS_2			0xA4

#if IS_ENABLED(CONFIG_USB_PHY_TUNING_QCOM)
#define ADDRESS_START eUSB2_RX_CONTROL
#define ADDRESS_END USB2_HS_DISCONNECT_THRESHOLD
#define TUNE_BUF_COUNT 20
#define TUNE_BUF_SIZE 25
#define TUNE_MAX_NXP 17
#define TUNE_MAX_TI 12

static u8 tune_map_nxp[TUNE_MAX_NXP] = {
	RESET_CONTROL,
	LINK_CONTROL1,
	LINK_CONTROL2,
	eUSB2_RX_CONTROL,
	eUSB2_TX_CONTROL,
	USB2_RX_CONTROL,
	USB2_TX_CONTROL1,
	USB2_TX_CONTROL2,
	USB2_HS_TERMINATION,
	USB2_HS_DISCONNECT_THRESHOLD,
	RAP_SIGNATURE,
	DEVICE_STATUS,
	LINK_STATUS,
	REVISION_ID,
	CHIP_ID_0,
	CHIP_ID_1,
	CHIP_ID_2,
};

static u8 tune_map_ti[TUNE_MAX_TI] = {
	GPIO0_CONFIG,
	GPIO1_CONFIG,
	UART_PORT1,
	EXTRA_PORT1,
	REV_ID,
	GLOBAL_CONFIG,
	INT_ENABLE_1,
	INT_ENABLE_2,
	BC_CONTROL,
	BC_STATUS_1,
	INT_STATUS_1,
	INT_STATUS_2,
};
#endif

enum eusb2_repeater_type {
	TI_REPEATER,
	NXP_REPEATER,
};

struct i2c_repeater_chip {
	enum eusb2_repeater_type repeater_type;
};

struct eusb2_repeater {
	struct device			*dev;
	struct usb_repeater		ur;
	struct regmap			*regmap;
	const struct i2c_repeater_chip	*chip;
	u16				reg_base;
	struct regulator		*vdd18;
	struct regulator		*vdd3;
	bool				power_enabled;

	struct gpio_desc		*reset_gpiod;
	int				reset_gpio_irq;
	u8				*param_override_seq;
	u8				param_override_seq_cnt;
#if IS_ENABLED(CONFIG_USB_NOTIFIER)
	u8				*param_host_override_seq;
	u8				param_host_override_seq_cnt;
#endif
#if IS_ENABLED(CONFIG_USB_PHY_TUNING_QCOM)
	struct mutex	er_tune_lock;
	int				tune_buf_cnt;
	u8				tune_buf[TUNE_BUF_COUNT][2];
	bool			er_tune_init_done;
#endif
};

static const struct regmap_config eusb2_i2c_regmap = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0xff,
};

#if IS_ENABLED(CONFIG_USB_PHY_TUNING_QCOM)
	struct eusb2_repeater *ter = NULL;
#endif

#undef dev_dbg
#define dev_dbg dev_err

#if IS_ENABLED(CONFIG_USB_NOTIFIER)
static void eusb2_repeater_update_seq(struct eusb2_repeater *er, u8 *seq, u8 cnt)
{
	int i, j, ret;

	dev_dbg(er->ur.dev, "%s %s mode param override seq count:%d\n",
		er->chip->repeater_type ? "NXP":"TI", er->ur.is_host ? "HOST":"CLIENT", cnt);
	cnt /= 4;
	for (i = 0; i < cnt; i = i+2) {
		for (j = 0; j < 3; j++) {
			ret = regmap_write(er->regmap, seq[i * 4 + 7], seq[i * 4 + 3]);
			if (ret < 0)
				dev_err(er->dev, "failed to write 0x%02x to reg: 0x%02x ret=%d\n",
					seq[i * 4 + 3], seq[i * 4 + 7], ret);
			else {
				dev_dbg(er->ur.dev, "write 0x%02x to 0x%02x\n", seq[i * 4 + 3], seq[i * 4 + 7]);
				break;
			}
		}
	}
}
#else
static int eusb2_i2c_read_reg(struct eusb2_repeater *er, u8 reg, u8 *val)
{
	int ret;
	unsigned int reg_val;

	ret = regmap_read(er->regmap, reg, &reg_val);
	if (ret < 0) {
		dev_err(er->dev, "Failed to read reg:0x%02x ret=%d\n", reg, ret);
		return ret;
	}

	*val = reg_val;
	dev_dbg(er->dev, "read reg:0x%02x val:0x%02x\n", reg, *val);

	return 0;
}

static int eusb2_i2c_write_reg(struct eusb2_repeater *er, u8 reg, u8 mask, u8 val)
{
	int ret;
	u8 reg_val;

	ret = eusb2_i2c_read_reg(er, reg, &reg_val);
	if (ret)
		return ret;

	val |= (reg_val & mask);
	ret = regmap_write(er->regmap, reg, val);
	if (ret < 0) {
		dev_err(er->dev, "failed to write 0x%02x to reg: 0x%02x ret=%d\n", val, reg, ret);
		return ret;
	}

	dev_dbg(er->dev, "write reg:0x%02x val:0x%02x\n", reg, val);

	return 0;
}

static void eusb2_repeater_update_seq(struct eusb2_repeater *er, u8 *seq, u8 cnt)
{
	int i;
	u8 mask = 0xFF;

	dev_dbg(er->ur.dev, "param override seq count:%d\n", cnt);
	for (i = 0; i < cnt; i = i+2) {
		dev_dbg(er->ur.dev, "write 0x%02x to 0x%02x\n", seq[i], seq[i+1]);
		eusb2_i2c_write_reg(er, seq[i+1], mask, seq[i]);
	}
}
#endif

#if IS_ENABLED(CONFIG_USB_PHY_TUNING_QCOM)
static void eusb2_repeater_tune_buf_init(void)
{
	int i;
	for (i = 0; i < TUNE_BUF_COUNT; i++) {
		ter->tune_buf[i][0] = ter->tune_buf[i][1] = 0;
	}
}

static void eusb2_repeater_tune_set(void)
{
	int i, j, ret;
	unsigned int reg_val;

	mutex_lock(&ter->er_tune_lock);
	for (i = 0; i < ter->tune_buf_cnt; i++) {
		for (j = 0; j < 3; j++) {
			if (!ter->ur.is_host && ter->chip->repeater_type == NXP_REPEATER &&
				ter->tune_buf[i][0] == 0x2 && ter->tune_buf[i][1] == 0x03) {
				pr_info("%s(): skip host test mode setting in USB client mode\n");
				break;
			}
			ret = regmap_write(ter->regmap, ter->tune_buf[i][0], ter->tune_buf[i][1]);
			if (ret < 0)
				dev_err(ter->dev, "failed to write 0x%02x to reg: 0x%02x ret=%d\n",
					ter->tune_buf[i][1], ter->tune_buf[i][0], ret);
			else
				break;
		}
		usleep_range(1, 10);
		for (j = 0; j < 3; j++) {
			ret = regmap_read(ter->regmap, ter->tune_buf[i][0], &reg_val);
			if (ret < 0)
				dev_err(ter->dev, "Failed to read reg:0x%02x ret=%d\n", ter->tune_buf[i][0], ret);
			else
				break;
		}
		pr_info("%s(): [%d] 0x%x 0x%x (%d/%d)\n", __func__, i, ter->tune_buf[i][0],
			reg_val, ter->tune_buf_cnt, TUNE_BUF_COUNT);
		usleep_range(1, 2);
	}
	mutex_unlock(&ter->er_tune_lock);
}

static ssize_t eusb2_repeater_tune_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	char str[(TUNE_BUF_SIZE * TUNE_BUF_COUNT) + 35] = {0, };
	int i, ret;
	unsigned int reg_val;

	if (!ter) {
		pr_err("eusb2 repeater is NULL\n");
		return -ENODEV;
	}
	mutex_lock(&ter->er_tune_lock);
	sprintf(str, "\n Address Value - %s\n", ter->chip->repeater_type ? "NXP":"TI");
	if (ter->chip->repeater_type == NXP_REPEATER) {
		for (i = 0; i < TUNE_MAX_NXP; i++) {
			ret = regmap_read(ter->regmap, tune_map_nxp[i], &reg_val);
			if (ret < 0) {
				dev_err(ter->dev, "Failed to read reg:0x%02x ret=%d\n", tune_map_nxp[i], ret);
				mutex_unlock(&ter->er_tune_lock);
				return sprintf(buf, "Failed to read reg\n");
			}
			sprintf(str, "%s  0x%2x   0x%2x\n", str, tune_map_nxp[i], reg_val);
		}
	} else {
		for (i = 0; i < TUNE_MAX_TI; i++) {
			ret = regmap_read(ter->regmap, tune_map_ti[i], &reg_val);
			if (ret < 0) {
				dev_err(ter->dev, "Failed to read reg:0x%02x ret=%d\n", tune_map_ti[i], ret);
				mutex_unlock(&ter->er_tune_lock);
				return sprintf(buf, "Failed to read reg\n");
			}
			sprintf(str, "%s  0x%2x   0x%2x\n", str, tune_map_ti[i], reg_val);
		}
	}
	mutex_unlock(&ter->er_tune_lock);

	return sprintf(buf, "%s\n", str);
}

static ssize_t eusb2_repeater_tune_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	u8 reg, val;
	int i, ret;
	unsigned int reg_val;

	pr_info("%s buf=%s\n", __func__, buf);
	if (!ter) {
		pr_err("eusb2 repeater is NULL\n");
		return -ENODEV;
	}
	sscanf(buf, "%x %x", &reg, &val);
	mutex_lock(&ter->er_tune_lock);

	for (i = 0; i < ter->tune_buf_cnt; i++) {
		if (ter->tune_buf[i][0] == reg) {
			ret = regmap_write(ter->regmap, reg, val);
			if (ret < 0) {
				dev_err(ter->dev, "failed to write 0x%02x to reg: 0x%02x ret=%d\n", val, reg, ret);
				mutex_unlock(&ter->er_tune_lock);
				return ret;
			}
			ter->tune_buf[i][1] = val;
			usleep_range(1, 2);
			ret = regmap_read(ter->regmap, reg, &reg_val);
			if (ret < 0) {
				dev_err(ter->dev, "Failed to read reg:0x%02x ret=%d\n", reg, ret);
				mutex_unlock(&ter->er_tune_lock);
				return ret;
			}
			pr_info("%s(): [%d] 0x%x 0x%x (%d/%d)\n", __func__, i, reg,
				reg_val, ter->tune_buf_cnt, TUNE_BUF_COUNT);
			mutex_unlock(&ter->er_tune_lock);
			return size;
		}
	}
	if (ter->tune_buf_cnt < TUNE_BUF_COUNT) {
		ret = regmap_write(ter->regmap, reg, val);
		if (ret < 0) {
			dev_err(ter->dev, "failed to write 0x%02x to reg: 0x%02x ret=%d\n", val, reg, ret);
			mutex_unlock(&ter->er_tune_lock);
			return ret;
		}
		ter->tune_buf[i][0] = reg;
		ter->tune_buf[i][1] = val;
		usleep_range(1, 2);
		ret = regmap_read(ter->regmap, reg, &reg_val);
		if (ret < 0) {
			dev_err(ter->dev, "Failed to read reg:0x%02x ret=%d\n", reg, ret);
			mutex_unlock(&ter->er_tune_lock);
			return ret;
		}
		pr_info("%s(): [%d] 0x%x 0x%x (%d/%d)\n", __func__, i, reg,
			reg_val, ter->tune_buf_cnt, TUNE_BUF_COUNT);
		ter->tune_buf_cnt++;
	} else
		pr_info("%s(): tuning count is full\n", __func__);

	mutex_unlock(&ter->er_tune_lock);

	return size;
}

static DEVICE_ATTR_RW(eusb2_repeater_tune);
					       
static struct attribute *eusb2_repeater_attributes[] = {
	&dev_attr_eusb2_repeater_tune.attr,
	NULL
};

const struct attribute_group eusb2_repeater_sysfs_group = {
	.attrs = eusb2_repeater_attributes,
};
#endif

static int eusb2_repeater_power(struct eusb2_repeater *er, bool on)
{
	int ret = 0;

	dev_dbg(er->ur.dev, "%s turn %s regulators. power_enabled:%d\n",
			__func__, on ? "on" : "off", er->power_enabled);

	if (er->power_enabled == on) {
		dev_dbg(er->ur.dev, "regulators are already ON.\n");
		return 0;
	}

	if (!on)
		goto disable_vdd3;

	ret = regulator_set_load(er->vdd18, EUSB2_1P8_HPM_LOAD);
	if (ret < 0) {
		dev_err(er->ur.dev, "Unable to set HPM of vdd12:%d\n", ret);
		goto err_vdd18;
	}

	ret = regulator_set_voltage(er->vdd18, EUSB2_1P8_VOL_MIN,
						EUSB2_1P8_VOL_MAX);
	if (ret) {
		dev_err(er->ur.dev,
				"Unable to set voltage for vdd18:%d\n", ret);
		goto put_vdd18_lpm;
	}

	ret = regulator_enable(er->vdd18);
	if (ret) {
		dev_err(er->ur.dev, "Unable to enable vdd18:%d\n", ret);
		goto unset_vdd18;
	}

	ret = regulator_set_load(er->vdd3, EUSB2_3P0_HPM_LOAD);
	if (ret < 0) {
		dev_err(er->ur.dev, "Unable to set HPM of vdd3:%d\n", ret);
		goto disable_vdd18;
	}

	ret = regulator_set_voltage(er->vdd3, EUSB2_3P0_VOL_MIN,
						EUSB2_3P0_VOL_MAX);
	if (ret) {
		dev_err(er->ur.dev,
				"Unable to set voltage for vdd3:%d\n", ret);
		goto put_vdd3_lpm;
	}

	ret = regulator_enable(er->vdd3);
	if (ret) {
		dev_err(er->ur.dev, "Unable to enable vdd3:%d\n", ret);
		goto unset_vdd3;
	}

	er->power_enabled = true;
	pr_debug("%s(): eUSB2 repeater egulators are turned ON.\n", __func__);
	return ret;

disable_vdd3:
	ret = regulator_disable(er->vdd3);
	if (ret)
		dev_err(er->ur.dev, "Unable to disable vdd3:%d\n", ret);

unset_vdd3:
	ret = regulator_set_voltage(er->vdd3, 0, EUSB2_3P0_VOL_MAX);
	if (ret)
		dev_err(er->ur.dev,
			"Unable to set (0) voltage for vdd3:%d\n", ret);

put_vdd3_lpm:
	ret = regulator_set_load(er->vdd3, 0);
	if (ret < 0)
		dev_err(er->ur.dev, "Unable to set (0) HPM of vdd3\n");

disable_vdd18:
	ret = regulator_disable(er->vdd18);
	if (ret)
		dev_err(er->ur.dev, "Unable to disable vdd18:%d\n", ret);

unset_vdd18:
	ret = regulator_set_voltage(er->vdd18, 0, EUSB2_1P8_VOL_MAX);
	if (ret)
		dev_err(er->ur.dev,
			"Unable to set (0) voltage for vdd18:%d\n", ret);

put_vdd18_lpm:
	ret = regulator_set_load(er->vdd18, 0);
	if (ret < 0)
		dev_err(er->ur.dev, "Unable to set LPM of vdd18\n");

	/* case handling when regulator turning on failed */
	if (!er->power_enabled)
		return -EINVAL;

err_vdd18:
	er->power_enabled = false;
	dev_dbg(er->ur.dev, "eUSB2 repeater's regulators are turned OFF.\n");
	return ret;
}

static int eusb2_repeater_init(struct usb_repeater *ur)
{
	struct eusb2_repeater *er =
			container_of(ur, struct eusb2_repeater, ur);

	/* override init sequence using devicetree based values */
#if IS_ENABLED(CONFIG_USB_NOTIFIER)
	if (er->param_host_override_seq_cnt && er->ur.is_host)
		eusb2_repeater_update_seq(er, er->param_host_override_seq,
					er->param_host_override_seq_cnt);
	else
#endif
	if (er->param_override_seq_cnt)
		eusb2_repeater_update_seq(er, er->param_override_seq,
					er->param_override_seq_cnt);
#if IS_ENABLED(CONFIG_USB_PHY_TUNING_QCOM)
	if (er->tune_buf_cnt && er->er_tune_init_done)
		eusb2_repeater_tune_set();
#endif
	dev_info(er->ur.dev, "eUSB2 repeater init\n");

	return 0;
}

static int eusb2_repeater_reset(struct usb_repeater *ur, bool bring_out_of_reset)
{
	struct eusb2_repeater *er =
			container_of(ur, struct eusb2_repeater, ur);

	dev_dbg(ur->dev, "reset gpio:%s\n",
			bring_out_of_reset ? "assert" : "deassert");
	gpiod_set_value_cansleep(er->reset_gpiod, bring_out_of_reset);
	return 0;
}

static int eusb2_repeater_powerup(struct usb_repeater *ur)
{
	struct eusb2_repeater *er =
			container_of(ur, struct eusb2_repeater, ur);

	return eusb2_repeater_power(er, true);
}

static int eusb2_repeater_powerdown(struct usb_repeater *ur)
{
	struct eusb2_repeater *er =
			container_of(ur, struct eusb2_repeater, ur);

	return eusb2_repeater_power(er, false);
}

static irqreturn_t eusb2_reset_gpio_irq_handler(int irq, void *dev_id)
{
	/*
	 * This IRQ handler is just returning IRQ_HANDLED to notify
	 * interrupt framework to clear the interrupt.
	 */
	struct eusb2_repeater *er = dev_id;

	dev_dbg(er->ur.dev, "reset gpio interrupt handled\n");
	return IRQ_HANDLED;
}

static struct i2c_repeater_chip repeater_chip[] = {
	[NXP_REPEATER] = {
		.repeater_type = NXP_REPEATER,
	},
	[TI_REPEATER] = {
		.repeater_type = TI_REPEATER,
	}
};

static const struct of_device_id eusb2_repeater_id_table[] = {
	{
		.compatible = "nxp,eusb2-repeater",
		.data = &repeater_chip[NXP_REPEATER]
	},
	{
		.compatible = "ti,eusb2-repeater",
		.data = &repeater_chip[TI_REPEATER]
	},
	{ },
};
MODULE_DEVICE_TABLE(of, eusb2_repeater_id_table);

static int eusb2_repeater_i2c_probe(struct i2c_client *client)
{
	struct eusb2_repeater *er;
	struct device *dev = &client->dev;
	const struct of_device_id *match;
	int ret = 0, num_elem;
#if IS_ENABLED(CONFIG_USB_PHY_TUNING_QCOM)
	struct device *eusb2_repeater_device;
#endif

	pr_info("%s\n", __func__);
	er = devm_kzalloc(dev, sizeof(*er), GFP_KERNEL);
	if (!er) {
		ret = -ENOMEM;
		goto err_probe;
	}

	er->dev = dev;
	match = of_match_node(eusb2_repeater_id_table, dev->of_node);
	er->chip = match->data;

	er->regmap = devm_regmap_init_i2c(client, &eusb2_i2c_regmap);
	if (!er->regmap) {
		dev_err(dev, "failed to allocate register map\n");
		ret = -EINVAL;
		goto err_probe;
	}

	devm_regmap_qti_debugfs_register(er->dev, er->regmap);
	i2c_set_clientdata(client, er);

	ret = of_property_read_u16(dev->of_node, "reg", &er->reg_base);
	if (ret < 0) {
		dev_err(dev, "failed to get reg base address:%d\n", ret);
		goto err_probe;
	}

	er->vdd3 = devm_regulator_get(dev, "vdd3");
	if (IS_ERR(er->vdd3)) {
		dev_err(dev, "unable to get vdd3 supply\n");
		ret = PTR_ERR(er->vdd3);
		goto err_probe;
	}

	er->vdd18 = devm_regulator_get(dev, "vdd18");
	if (IS_ERR(er->vdd18)) {
		dev_err(dev, "unable to get vdd18 supply\n");
		ret = PTR_ERR(er->vdd18);
		goto err_probe;
	}

	er->reset_gpiod = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(er->reset_gpiod)) {
		ret = PTR_ERR(er->reset_gpiod);
		goto err_probe;
	}

	er->reset_gpio_irq = of_irq_get_byname(dev->of_node, "eusb2_rptr_reset_gpio_irq");
	if (er->reset_gpio_irq < 0) {
		dev_err(dev, "failed to get reset gpio IRQ\n");
		ret = er->reset_gpio_irq;
		goto err_probe;
	}

	ret = devm_request_irq(dev, er->reset_gpio_irq,
			eusb2_reset_gpio_irq_handler, IRQF_TRIGGER_RISING,
			client->name, er);
	if (ret < 0) {
		dev_err(dev, "failed to request reset gpio irq\n");
		goto err_probe;
	}

	num_elem = of_property_count_elems_of_size(dev->of_node, "qcom,param-override-seq",
				sizeof(*er->param_override_seq));
	if (num_elem > 0) {
		if (num_elem % 2) {
			dev_err(dev, "invalid param_override_seq_len\n");
			ret = -EINVAL;
			goto err_probe;
		}

		er->param_override_seq_cnt = num_elem;
		er->param_override_seq = devm_kcalloc(dev,
				er->param_override_seq_cnt,
				sizeof(*er->param_override_seq), GFP_KERNEL);
		if (!er->param_override_seq) {
			ret = -ENOMEM;
			goto err_probe;
		}

		ret = of_property_read_u8_array(dev->of_node,
				"qcom,param-override-seq",
				er->param_override_seq,
				er->param_override_seq_cnt);
		if (ret) {
			dev_err(dev, "qcom,param-override-seq read failed %d\n",
									ret);
			goto err_probe;
		}
	}

#if IS_ENABLED(CONFIG_USB_NOTIFIER)
	num_elem = of_property_count_elems_of_size(dev->of_node, "qcom,param-host-override-seq",
				sizeof(*er->param_host_override_seq));
	if (num_elem > 0) {
		if (num_elem % 2) {
			dev_err(dev, "invalid param_host_override_seq_len\n");
			ret = -EINVAL;
			goto err_probe;
		}

		er->param_host_override_seq_cnt = num_elem;
		er->param_host_override_seq = devm_kcalloc(dev,
				er->param_host_override_seq_cnt,
				sizeof(*er->param_host_override_seq), GFP_KERNEL);
		if (!er->param_host_override_seq) {
			ret = -ENOMEM;
			goto err_probe;
		}

		ret = of_property_read_u8_array(dev->of_node,
				"qcom,param-host-override-seq",
				er->param_host_override_seq,
				er->param_host_override_seq_cnt);
		if (ret) {
			dev_err(dev, "qcom,param-host-override-seq read failed %d\n",
									ret);
			goto err_probe;
		}
	}
#endif

	er->ur.dev = dev;

	er->ur.init		= eusb2_repeater_init;
	er->ur.reset		= eusb2_repeater_reset;
	er->ur.powerup		= eusb2_repeater_powerup;
	er->ur.powerdown	= eusb2_repeater_powerdown;

	ret = usb_add_repeater_dev(&er->ur);
	if (ret)
		goto err_probe;

#if IS_ENABLED(CONFIG_USB_PHY_TUNING_QCOM)
	ter = er;
	er->tune_buf_cnt = 0;
	er->er_tune_init_done = true;
	eusb2_repeater_tune_buf_init();
	mutex_init(&er->er_tune_lock);
	eusb2_repeater_device = sec_device_create(NULL, "usb_repeater");
	if (IS_ERR(eusb2_repeater_device))
		pr_err("%s Failed to create device(usb_repeater)!\n", __func__);


	ret = sysfs_create_group(&eusb2_repeater_device->kobj, &eusb2_repeater_sysfs_group);
	if (ret)
		pr_err("%s: usb_repeater sysfs_create_group fail, ret %d", __func__, ret);
#endif
	pr_info("%s %s done\n", __func__, er->chip->repeater_type ? "NXP":"TI");
	return 0;

err_probe:
	pr_info("%s failed. ret(%d)\n", __func__, ret);
	return ret;
}

static int eusb2_repeater_i2c_remove(struct i2c_client *client)
{
	struct eusb2_repeater *er = i2c_get_clientdata(client);

	if (!er)
		return 0;
#if IS_ENABLED(CONFIG_USB_PHY_TUNING_QCOM)
	mutex_destroy(&er->er_tune_lock);
#endif
	usb_remove_repeater_dev(&er->ur);
	eusb2_repeater_power(er, false);
	return 0;
}

static struct i2c_driver eusb2_i2c_repeater_driver = {
	.probe_new	= eusb2_repeater_i2c_probe,
	.remove		= eusb2_repeater_i2c_remove,
	.driver = {
		.name	= "eusb2-repeater",
		.of_match_table = of_match_ptr(eusb2_repeater_id_table),
	},
};

module_i2c_driver(eusb2_i2c_repeater_driver);

MODULE_DESCRIPTION("eUSB2 i2c repeater driver");
MODULE_LICENSE("GPL v2");