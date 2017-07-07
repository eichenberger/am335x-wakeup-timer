#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/pm_wakeup.h>
#include <linux/clk.h>
#include <linux/interrupt.h>


#include "dmtimer.h"

struct wakeup_timer_priv {
	struct omap_dm_timer *timer;
	int freq_hz;
	int irq;
	u32 sleep_time_ms;

	struct platform_device *pdev;
};

static irqreturn_t omap_gpio_irq_handler(int irq, void *data)
{
	struct wakeup_timer_priv *priv = data;
	struct omap_dm_timer *timer = priv->timer;
	unsigned int status = omap_dm_timer_read_status(timer);

	omap_dm_timer_write_status(timer, status);
	omap_dm_timer_write_counter(timer, 0);
	omap_dm_timer_set_int_disable(timer, 2);

	pm_wakeup_event(&priv->pdev->dev, 0);
	return IRQ_HANDLED;
}

static int start_timer(struct wakeup_timer_priv *priv)
{
	struct omap_dm_timer *timer = priv->timer;
	int freq_hz = priv->freq_hz;
	u32 sleep_time_ms = priv->sleep_time_ms;
	u32 sleep_clocks;
	u32 load;

	/* We only have 32 bits and we have to multiply with up to 20000 Hz therefore
	 * we need to round to seconds first if we have times > 40s.
	 * 40000 * 100000 = 4000000000 which still fits in 32 bits */
	if (sleep_time_ms > 40090) {
		sleep_clocks = (sleep_time_ms/1000) * freq_hz;
	}
	else {
		sleep_clocks = sleep_time_ms * freq_hz;
		sleep_clocks = sleep_clocks/1000; /* sleep_time_ms was in ms */
	}

	if (sleep_clocks > 0xFFFFFFFF) {
		printk(KERN_ERR "Can not start timer because sleep clocks (0x%016X) overflows 32bit\n", load);
		return -EINVAL;
	}

	load = 0xFFFFFFFF - sleep_clocks;

	omap_dm_timer_set_int_enable(timer, 2);

	printk(KERN_INFO "Set load register to: 0x%08X\n", load);
	omap_dm_timer_set_load_start(timer, 0, load);

	return 0;

}

static ssize_t trigger_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct wakeup_timer_priv *wakeup_timer = dev_get_drvdata(dev);

	dev_info(dev, "%p\n", wakeup_timer);

	if (wakeup_timer == NULL) {
		return -EINVAL;
	}

	if (count < 1) {
		dev_err(dev, "Invalid argument count\n");
		return -EINVAL;
	}

	if (buf[0] == '1') {
		start_timer(wakeup_timer);
	}

	return 1;
}

DEVICE_ATTR_WO(trigger);

static ssize_t sleep_time_ms_show(struct device *dev,
			      struct device_attribute *attr,
			      char *buf)
{
	struct wakeup_timer_priv *wakeup_timer = dev_get_drvdata(dev);
	return sprintf(buf, "%u\n", wakeup_timer->sleep_time_ms);
}

static ssize_t sleep_time_ms_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct wakeup_timer_priv *wakeup_timer = dev_get_drvdata(dev);
	int ret;

	ret = kstrtou32(buf, 0, &wakeup_timer->sleep_time_ms);
	if (ret) {
		dev_err(dev, "Can not parse input\n");
		return -EINVAL;
	}

	if (wakeup_timer->sleep_time_ms < 8) {
		dev_err(dev, "Minimal sleep time is 8ms\n");
		return -EINVAL;
	}

	if (wakeup_timer->sleep_time_ms > (12*3600*1000)) {
		dev_err(dev, "Maximum sleep time supported are 12h\n");
		return -EINVAL;
	}

	return count;
}

DEVICE_ATTR_RW(sleep_time_ms);

static struct attribute *wakeup_timer_attrs[] = {
	&dev_attr_trigger.attr,
	&dev_attr_sleep_time_ms.attr,
	NULL
};
ATTRIBUTE_GROUPS(wakeup_timer);

static int wakeup_timer_probe(struct platform_device *pdev)
{
	struct wakeup_timer_priv *wakeup_timer;
	struct omap_dm_timer *timer;
	struct clk *timer_clk;
	unsigned int rate;
	struct device_node *node;
	int ret;

	dev_info(&pdev->dev, "%s\n", __func__);

	wakeup_timer = devm_kzalloc(&pdev->dev, sizeof(*wakeup_timer), GFP_KERNEL);
	if (!wakeup_timer)
		return -ENOMEM;

	node = of_parse_phandle(pdev->dev.of_node, "timer", 0);
	if (node != NULL) {
		dev_info(&pdev->dev, "Request timer by node\n");
		timer = omap_dm_timer_request_by_node(node);
		if (timer == NULL) {
			return -EBUSY;
		}
	}
	else {
		dev_info(&pdev->dev, "Request any free timer\n");
		timer = omap_dm_timer_request();
		if (timer == NULL) {
			return -EBUSY;
		}
	}

	wakeup_timer->timer = timer;
	wakeup_timer->pdev = pdev;
	wakeup_timer->sleep_time_ms = 2000;

	omap_dm_timer_set_source(timer, OMAP_TIMER_SRC_SYS_CLK);

	timer_clk = omap_dm_timer_get_fclk(timer);

	rate = clk_get_rate(timer_clk);
	dev_info(&pdev->dev, "timer rate %d\n", rate);
	switch (rate) {
		case 25000000:
			/* Clock is 97656 kHz */
			omap_dm_timer_set_prescaler(timer, 7);
			wakeup_timer->freq_hz = 97656;
			break;
		case 32786:
			omap_dm_timer_set_prescaler(timer, 1);
			wakeup_timer->freq_hz = 16393;
			break;
		default:
			dev_err(&pdev->dev, "Rate of %d is not supported by this driver\n", rate);
			goto error;
	}


	wakeup_timer->irq = omap_dm_timer_get_irq(timer);
	if (wakeup_timer->irq <= 0) {
		dev_err(&pdev->dev, "Timer does not have any interrupt\n");
		goto error;
	}

	device_set_wakeup_capable(&pdev->dev, true);
	ret = device_wakeup_enable(&pdev->dev);
	if (ret) {
		dev_err(&pdev->dev, "Could not enable wakeup source %d\n", ret);
	}

	ret = devm_request_irq(&pdev->dev, wakeup_timer->irq, omap_gpio_irq_handler,
			       IRQF_NO_SUSPEND, "wakeup-timer", wakeup_timer);

	ret = sysfs_create_groups(&pdev->dev.kobj, wakeup_timer_groups);
	if (ret) {
		dev_err(&pdev->dev, "Could not create sysfs group\n");
	}

	platform_set_drvdata(pdev, wakeup_timer);

	return 0;

error:
	if (timer) {
		omap_dm_timer_free(timer);
		timer = NULL;
	}

	return -EINVAL;
}

static int __exit wakeup_timer_remove(struct platform_device *pdev)
{
	struct wakeup_timer_priv *wakeup_timer = platform_get_drvdata(pdev);

	dev_info(&pdev->dev, "%s\n", __func__);

	sysfs_remove_groups(&pdev->dev.kobj, wakeup_timer_groups);

	device_wakeup_disable(&pdev->dev);

	devm_free_irq(&pdev->dev, wakeup_timer->irq, wakeup_timer);

	omap_dm_timer_free(wakeup_timer->timer);

	devm_kfree(&pdev->dev, wakeup_timer);

	return 0;
}


static int wakeup_timer_suspend(struct device *dev)
{
	struct wakeup_timer_priv *wakeup_timer = dev_get_drvdata(dev);

	start_timer(wakeup_timer);

	return 0;
}


static int wakeup_timer_resume(struct device *dev)
{
	dev_info(dev, "%s\n", __func__);
	return 0;
}

static const struct dev_pm_ops wakeup_timer_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(wakeup_timer_suspend, wakeup_timer_resume)
};

static const struct platform_device_id wakeup_timer_id_table[] = {
	{
		.name	= "wakeup-timer",
	}, {
		/* sentinel */
	}
};
MODULE_DEVICE_TABLE(platform, wakeup_timer_id_table);

static const struct of_device_id wakeup_timer_of_match[] = {
	{
		.compatible	= "wakeup-timer",
	}, {
		/* sentinel */
	}
};
MODULE_DEVICE_TABLE(of, wakeup_timer_of_match);


static struct platform_driver wakeup_timer_driver = {
	.probe		= wakeup_timer_probe,
	.remove		= __exit_p(wakeup_timer_remove),
	.driver		= {
		.name	= "wakeup-timer",
		.pm	= &wakeup_timer_pm_ops,
		.of_match_table = wakeup_timer_of_match,
	},
	.id_table	= wakeup_timer_id_table,
};

module_platform_driver(wakeup_timer_driver);

MODULE_ALIAS("platform:wakeup_timer");
MODULE_AUTHOR("Stefan Eichenberger");
MODULE_LICENSE("GPL");
