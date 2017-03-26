/*
 * Copyright (c) 2017, Cosmin Tanislav(demonsingur@gmail.com).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/regulator/consumer.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/pwm.h>
#include <linux/of_gpio.h>
#include <linux/err.h>

#include "../staging/android/timed_output.h"

#define PWM_PERCENT_MIN     0
#define PWM_PERCENT_MAX     100
#define PWM_FREQUENCY       30000
#define PWM_TIMEOUT         15000

struct isa1000_vib {
	struct platform_device *pdev;
	struct pwm_device *pwm_dev;

	struct hrtimer vib_timer;
	struct timed_output_dev timed_dev;
	struct work_struct work;

	struct mutex lock;

	struct regulator *regulator_vdd;
	unsigned int pwm_frequency;
	int pwm_duty_percent;
	int enable_gpio;
	int timeout;
	bool state;
};

static struct isa1000_vib vib_dev = {
	.pwm_frequency = PWM_FREQUENCY,
	.pwm_duty_percent = PWM_PERCENT_MAX,
	.timeout = PWM_TIMEOUT
};

static ssize_t isa1000_pwm_min_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", PWM_PERCENT_MIN);
}

static ssize_t isa1000_pwm_max_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", PWM_PERCENT_MAX);
}

static ssize_t isa1000_pwm_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct timed_output_dev *timed_dev = dev_get_drvdata(dev);
	struct isa1000_vib *vib =
			container_of(timed_dev, struct isa1000_vib, timed_dev);

	return sprintf(buf, "%d\n", vib->pwm_duty_percent);
}

static ssize_t isa1000_pwm_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct timed_output_dev *timed_dev = dev_get_drvdata(dev);
	struct isa1000_vib *vib =
			container_of(timed_dev, struct isa1000_vib, timed_dev);
	int tmp;

	sscanf(buf, "%d", &tmp);

	if (tmp > PWM_PERCENT_MAX)
		tmp = PWM_PERCENT_MAX;
	else if (tmp < PWM_PERCENT_MIN)
		tmp = PWM_PERCENT_MIN;

	vib->pwm_duty_percent = tmp;

	return size;
}

static struct device_attribute isa1000_device_attrs[] = {
	__ATTR(vtg_min, S_IRUGO | S_IWUSR,
			isa1000_pwm_min_show,
			NULL),
	__ATTR(vtg_max, S_IRUGO | S_IWUSR,
			isa1000_pwm_max_show,
			NULL),
	__ATTR(vtg_level, S_IRUGO | S_IWUSR,
			isa1000_pwm_show,
			isa1000_pwm_store),
};

static int isa1000_config(struct isa1000_vib *vib, int percent) {
	unsigned int period_ns = NSEC_PER_SEC / vib->pwm_frequency;
	unsigned int duty_ns =
			(period_ns * (percent + PWM_PERCENT_MAX)) / (2 * PWM_PERCENT_MAX);

	return pwm_config(vib->pwm_dev, duty_ns, period_ns);
}

static int isa1000_set_state(struct isa1000_vib *vib, bool on)
{
	if (on) {
		int rc;

		rc = pwm_enable(vib->pwm_dev);
		if (rc < 0) {
			pr_err("%s: failed to enable pwm\n", __func__);
			return rc;
		}

		rc = isa1000_config(vib, vib->pwm_duty_percent);
		if (rc < 0) {
			pr_err("%s: failed to configure pwm\n", __func__);
			return rc;
		}

		gpio_set_value_cansleep(vib->enable_gpio, 1);
	} else {
		gpio_set_value_cansleep(vib->enable_gpio, 0);
		pwm_disable(vib->pwm_dev);
	}

	return 0;
}

static void isa1000_update(struct work_struct *work)
{
	struct isa1000_vib *vib = container_of(work, struct isa1000_vib, work);
	isa1000_set_state(vib, vib->state);
}

static enum hrtimer_restart isa1000_timer_func(struct hrtimer *timer)
{
	struct isa1000_vib *vib = container_of(timer,
			struct isa1000_vib, vib_timer);

	vib->state = 0;
	schedule_work(&vib->work);

	return HRTIMER_NORESTART;
}

static int isa1000_get_time(struct timed_output_dev *dev)
{
	struct isa1000_vib *vib = container_of(dev, struct isa1000_vib, timed_dev);

	if (hrtimer_active(&vib->vib_timer)) {
		ktime_t r = hrtimer_get_remaining(&vib->vib_timer);
		return (int) ktime_to_us(r);
	}

	return 0;
}

static void isa1000_enable(struct timed_output_dev *dev, int value)
{
	struct isa1000_vib *vib = container_of(dev, struct isa1000_vib, timed_dev);

	mutex_lock(&vib->lock);
	hrtimer_cancel(&vib->vib_timer);

	if (value == 0) {
		vib->state = 0;
	} else {
		vib->state = 1;
		value = value > vib->timeout ? vib->timeout : value;
		hrtimer_start(&vib->vib_timer,
				ktime_set(value / 1000, (value % 1000) * 1000000),
				HRTIMER_MODE_REL);
	}

	mutex_unlock(&vib->lock);
	schedule_work(&vib->work);
}

static int isa1000_probe(struct platform_device *pdev)
{
	struct isa1000_vib *vib;
	u32 temp_val;
	int rc;
	int i;

	platform_set_drvdata(pdev, &vib_dev);
	vib = (struct isa1000_vib *) platform_get_drvdata(pdev);

	vib->enable_gpio = of_get_named_gpio_flags(pdev->dev.of_node,
			"isa1000,enable-gpio", 0, NULL);
	if (vib->enable_gpio < 0) {
		rc = -ENODEV;
		dev_err(&pdev->dev, "%s: unable to get enable gpio\n", __func__);
		goto error;
	}

	vib->pwm_dev = of_pwm_get(pdev->dev.of_node, NULL);
	if (vib->pwm_dev < 0) {
		rc = PTR_ERR(vib->pwm_dev);
		dev_err(&pdev->dev, "%s: unable to get pwm device\n", __func__);
		goto error;
	}

	vib->regulator_vdd = devm_regulator_get(&pdev->dev, "vdd");
	if (IS_ERR(vib->regulator_vdd)) {
		rc = PTR_ERR(vib->regulator_vdd);
		dev_err(&pdev->dev, "%s: unable to get regulator\n", __func__);
		goto error;
	}

	rc = of_property_read_u32(pdev->dev.of_node,
			"isa1000,timeout-ms", &temp_val);
	if (!rc) {
		vib->timeout = temp_val;
	}

	rc = isa1000_config(vib, vib->pwm_duty_percent);
	if (rc < 0) {
		dev_err(&pdev->dev, "%s: failed to configure pwm\n", __func__);
		return rc;
	}

	rc = regulator_enable(vib->regulator_vdd);
	if (rc < 0) {
		dev_err(&pdev->dev, "%s: failed to enable regulator\n", __func__);
		goto error;
	}

	rc = gpio_request(vib->enable_gpio, "vibrator_en");
	if (rc < 0) {
		dev_err(&pdev->dev, "%s: failed to request gpio\n", __func__);
		goto error;
	}

	mutex_init(&vib->lock);
	INIT_WORK(&vib->work, isa1000_update);

	hrtimer_init(&vib->vib_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	vib->vib_timer.function = isa1000_timer_func;

	vib->timed_dev.name = "vibrator";
	vib->timed_dev.get_time = isa1000_get_time;
	vib->timed_dev.enable = isa1000_enable;

	rc = timed_output_dev_register(&vib->timed_dev);
	if (rc < 0) {
		dev_err(&pdev->dev,
				"%s: failed to register timed output device\n", __func__);
		goto error;
	}

	for (i = 0; i < ARRAY_SIZE(isa1000_device_attrs); i++) {
		rc = device_create_file(vib->timed_dev.dev, &isa1000_device_attrs[i]);
		if (rc < 0) {
			dev_err(&pdev->dev,
					"%s: failed to create sysfs attributes\n", __func__);
			goto error;
		}
	}

	return 0;

error:
	return rc;
}

static int isa1000_remove(struct platform_device *pdev)
{
	struct isa1000_vib *vib = dev_get_drvdata(&pdev->dev);

	timed_output_dev_unregister(&vib->timed_dev);
	hrtimer_cancel(&vib->vib_timer);
	cancel_work_sync(&vib->work);
	mutex_destroy(&vib->lock);
	gpio_free(vib->enable_gpio);

	return 0;
}

static struct of_device_id vibrator_match_table[] = {
	{ .compatible = "vibrator,isa1000", },
	{}
};

static struct platform_driver isa1000_driver = {
	.driver = {
		.name	= "vibrator,isa1000",
		.of_match_table = vibrator_match_table,
	},
	.probe		= isa1000_probe,
	.remove		= isa1000_remove,
};

static int __init isa1000_init(void) {
	return platform_driver_register(&isa1000_driver);
};
module_init(isa1000_init);

static void __exit isa1000_exit(void) {
	return platform_driver_unregister(&isa1000_driver);
};
module_exit(isa1000_exit);
