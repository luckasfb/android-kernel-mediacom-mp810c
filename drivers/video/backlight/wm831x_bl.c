/*
 * Backlight driver for Wolfson Microelectronics WM831x PMICs
 *
 * Copyright 2009 Wolfson Microelectonics plc
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/fb.h>
#include <linux/backlight.h>
#include <linux/slab.h>

#include <linux/mfd/wm831x/core.h>
#include <linux/mfd/wm831x/pdata.h>
#include <linux/mfd/wm831x/regulator.h>
#ifdef CONFIG_HAS_EARLYSUSPEND
#include <linux/earlysuspend.h>
#endif
#include <linux/delay.h>
#include <linux/ktime.h>
#define BL_SET   255
#define BL_MISC_VALUE 20
#define BL_INIT_VALUE 102
struct wm831x_backlight_data {
	struct wm831x *wm831x;
	int isink_reg;
	int current_brightness;
#ifdef CONFIG_HAS_EARLYSUSPEND
	struct 	early_suspend early_suspend;
	struct delayed_work work;
	int suspend_flag;
	int shutdown_flag;
#endif
};
#define TS_POLL_DELAY (10000*1000*1000)
int wm831x_bright = 0;
int max_tp = 0;
#ifdef CONFIG_HAS_EARLYSUSPEND
static struct backlight_device *gwm831x_bl;
static struct wm831x_backlight_data *gwm831x_data;
#endif
static int wm831x_backlight_set(struct backlight_device *bl, int brightness)
{
	struct wm831x_backlight_data *data = bl_get_data(bl);
	struct wm831x *wm831x = data->wm831x;
//	int power_up = !data->current_brightness && brightness;
//	int power_down = data->current_brightness && !brightness;
	int power_up;
	int power_down;
	int ret;
	int bright_tp;

	bright_tp =( max_tp*brightness)/BL_SET;
	power_up =!data->current_brightness && bright_tp;
	power_down = data->current_brightness && !bright_tp;
	if (power_up) {
		/* Enable the ISINK */
		ret = wm831x_set_bits(wm831x, data->isink_reg,
				      WM831X_CS1_ENA, WM831X_CS1_ENA);
		if (ret < 0)
			goto err;

		/* Enable the DC-DC */
		ret = wm831x_set_bits(wm831x, WM831X_DCDC_ENABLE,
				      WM831X_DC4_ENA, WM831X_DC4_ENA);
		if (ret < 0)
			goto err;
	}

	if (power_down) {
		/* DCDC first */
		ret = wm831x_set_bits(wm831x, WM831X_DCDC_ENABLE,
				      WM831X_DC4_ENA, 0);
		if (ret < 0)
			goto err;

		/* ISINK */
		ret = wm831x_set_bits(wm831x, data->isink_reg,
				      WM831X_CS1_DRIVE | WM831X_CS1_ENA, 0);
		if (ret < 0)
			goto err;
	}

	/* Set the new brightness */
	ret = wm831x_set_bits(wm831x, data->isink_reg,
			      WM831X_CS1_ISEL_MASK, bright_tp);
	if (ret < 0)
		goto err;

	if (power_up) {
		/* Drive current through the ISINK */
		ret = wm831x_set_bits(wm831x, data->isink_reg,
				      WM831X_CS1_DRIVE, WM831X_CS1_DRIVE);
		if (ret < 0)
			return ret;
	}

	data->current_brightness = brightness;

	return 0;

err:
	/* If we were in the middle of a power transition always shut down
	 * for safety.
	 */
	if (power_up || power_down) {
		wm831x_set_bits(wm831x, WM831X_DCDC_ENABLE, WM831X_DC4_ENA, 0);
		wm831x_set_bits(wm831x, data->isink_reg, WM831X_CS1_ENA, 0);
	}

	return ret;
}

static int wm831x_backlight_update_status(struct backlight_device *bl)
{
	int brightness = bl->props.brightness;
	if (brightness<=BL_MISC_VALUE) {
		brightness = 8*brightness;
	}
	else if (brightness<=BL_INIT_VALUE) {
		brightness = 31*brightness/41 + 145;
	}
	else {
		brightness = 33*brightness/153 + 200;
	}

	if(gwm831x_data->suspend_flag == 1)
		brightness = 0;
	if (gwm831x_data->shutdown_flag == 1)
		brightness = 0;
		
	if (bl->props.power != FB_BLANK_UNBLANK)
		brightness = 0;

	if (bl->props.fb_blank != FB_BLANK_UNBLANK)
		brightness = 0;

	if (bl->props.state & BL_CORE_SUSPENDED)
		brightness = 0;

	printk("backlight brightness=%d\n", brightness);

	return wm831x_backlight_set(bl, brightness);
}

static int wm831x_backlight_get_brightness(struct backlight_device *bl)
{
	struct wm831x_backlight_data *data = bl_get_data(bl);
	return data->current_brightness;
}

static struct backlight_ops wm831x_backlight_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.update_status	= wm831x_backlight_update_status,
	.get_brightness	= wm831x_backlight_get_brightness,
};
#ifdef CONFIG_HAS_EARLYSUSPEND
static void wm831x_bl_work(struct work_struct *work)
{
	//struct wm831x_backlight_data *wm831x_data = container_of(work, struct wm831x_backlight_data,
						   //work.work);
	backlight_update_status(gwm831x_bl);
}

static void wm831x_bl_suspend(struct early_suspend *h)
{
	struct wm831x_backlight_data *wm831x_data;
	wm831x_data = container_of(h, struct wm831x_backlight_data, early_suspend);
	wm831x_data->suspend_flag = 1;

	schedule_delayed_work(&wm831x_data->work, msecs_to_jiffies(100));		
}


static void wm831x_bl_resume(struct early_suspend *h)
{
	struct wm831x_backlight_data *wm831x_data;
	wm831x_data = container_of(h, struct wm831x_backlight_data, early_suspend);
	wm831x_data->suspend_flag = 0;
	
	schedule_delayed_work(&wm831x_data->work, msecs_to_jiffies(100));
}

#endif

int rk29_backlight_ctrl(int open)
{
	gwm831x_data->suspend_flag = !open;
	schedule_delayed_work(&gwm831x_data->work, 0);
}

static int wm831x_backlight_probe(struct platform_device *pdev)
{
	struct wm831x *wm831x = dev_get_drvdata(pdev->dev.parent);
	struct wm831x_pdata *wm831x_pdata;
	struct wm831x_backlight_pdata *pdata;
	struct wm831x_backlight_data *data;
	struct backlight_device *bl;
	struct backlight_properties props;
	int ret, i, max_isel, isink_reg, dcdc_cfg;

	/* We need platform data */
	if (pdev->dev.parent->platform_data) {
		wm831x_pdata = pdev->dev.parent->platform_data;
		pdata = wm831x_pdata->backlight;
	} else {
		pdata = NULL;
	}

	if (!pdata) {
		dev_err(&pdev->dev, "No platform data supplied\n");
		return -EINVAL;
	}

	/* Figure out the maximum current we can use */
	for (i = 0; i < WM831X_ISINK_MAX_ISEL; i++) {
		if (wm831x_isinkv_values[i] > pdata->max_uA)
			break;
	}

	if (i == 0) {
		dev_err(&pdev->dev, "Invalid max_uA: %duA\n", pdata->max_uA);
		return -EINVAL;
	}
	max_isel = i - 1;
	max_tp = max_isel;
	if (pdata->max_uA != wm831x_isinkv_values[max_isel])
		dev_warn(&pdev->dev,
			 "Maximum current is %duA not %duA as requested\n",
			 wm831x_isinkv_values[max_isel], pdata->max_uA);

	switch (pdata->isink) {
	case 1:
		isink_reg = WM831X_CURRENT_SINK_1;
		dcdc_cfg = 0;
		break;
	case 2:
		isink_reg = WM831X_CURRENT_SINK_2;
		dcdc_cfg = WM831X_DC4_FBSRC;
		break;
	default:
		dev_err(&pdev->dev, "Invalid ISINK %d\n", pdata->isink);
		return -EINVAL;
	}

	/* Configure the ISINK to use for feedback */
	ret = wm831x_reg_unlock(wm831x);
	if (ret < 0)
		return ret;

	ret = wm831x_set_bits(wm831x, WM831X_DC4_CONTROL, WM831X_DC4_FBSRC,
			      dcdc_cfg);

	wm831x_reg_lock(wm831x);
	if (ret < 0)
		return ret;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (data == NULL)
		return -ENOMEM;

	data->wm831x = wm831x;
	data->current_brightness = 0;
	data->isink_reg = isink_reg;

	props.max_brightness = max_isel;
	bl = backlight_device_register("wm831x", &pdev->dev, data,
				       &wm831x_backlight_ops);
	if (IS_ERR(bl)) {
		dev_err(&pdev->dev, "failed to register backlight\n");
		kfree(data);
		return PTR_ERR(bl);
	}

	bl->props.brightness = BL_INIT_VALUE;
	bl->props.max_brightness= BL_SET;

	platform_set_drvdata(pdev, bl);

#ifdef CONFIG_HAS_EARLYSUSPEND	
	data->early_suspend.level = ~0x0;
	data->early_suspend.suspend = wm831x_bl_suspend;
	data->early_suspend.resume = wm831x_bl_resume;
	register_early_suspend(&data->early_suspend);
	INIT_DELAYED_WORK(&data->work, wm831x_bl_work);
	gwm831x_bl = bl;
	gwm831x_data = data;
#endif


	/* Disable the DCDC if it was started so we can bootstrap */
	wm831x_set_bits(wm831x, WM831X_DCDC_ENABLE, WM831X_DC4_ENA, 0);

	//backlight_update_status(bl);
	schedule_delayed_work(&data->work, msecs_to_jiffies(100));

	return 0;
}

static int wm831x_backlight_remove(struct platform_device *pdev)
{
	struct backlight_device *bl = platform_get_drvdata(pdev);
	struct wm831x_backlight_data *data = bl_get_data(bl);

	backlight_device_unregister(bl);
#ifdef CONFIG_HAS_EARLYSUSPEND
	unregister_early_suspend(&data->early_suspend);
#endif 
	kfree(data);
	return 0;
}

static void wm831x_backlight_shutdown(struct platform_device *pdev)
{
	struct backlight_device *bl = platform_get_drvdata(pdev);
	struct wm831x_backlight_data *data = bl_get_data(bl);
	
	printk("enter %s\n", __func__);
	data->shutdown_flag = 1;
	wm831x_backlight_update_status(bl);
	return;
}

static struct platform_driver wm831x_backlight_driver = {
	.driver		= {
		.name	= "wm831x-backlight",
		.owner	= THIS_MODULE,
	},
	.probe		= wm831x_backlight_probe,
	.remove		= wm831x_backlight_remove,
	.shutdown	= wm831x_backlight_shutdown,
};

static int __init wm831x_backlight_init(void)
{
	return platform_driver_register(&wm831x_backlight_driver);
}
module_init(wm831x_backlight_init);

static void __exit wm831x_backlight_exit(void)
{
	platform_driver_unregister(&wm831x_backlight_driver);
}
module_exit(wm831x_backlight_exit);

MODULE_DESCRIPTION("Backlight Driver for WM831x PMICs");
MODULE_AUTHOR("Mark Brown <broonie@opensource.wolfsonmicro.com");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:wm831x-backlight");
