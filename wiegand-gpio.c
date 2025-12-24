// SPDX-License-Identifier: GPL-2.0
/*
 * wiegand-gpio.c
 *
 * GPIO-based Wiegand reader with IRQ edge capture.
 * Updated for WB8 kernel 6.8 (AArch64) and defaults to A2/A1 inputs.
 */

#include <linux/gpio.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/kobject.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

#define MAX_WIEGAND_BYTES	16
#define MIN_PULSE_INTERVAL_NS	(100ULL * 1000ULL)
#define FRAME_TIMEOUT_NS	(50ULL * 1000ULL * 1000ULL)
#define TIMER_PERIOD_NS		(10ULL * 1000ULL * 1000ULL)

static ushort gpio_wiegand_d0 = 228; /* WB8 A2 IN */
static ushort gpio_wiegand_d1 = 233; /* WB8 A1 IN */

module_param_named(D0, gpio_wiegand_d0, ushort, 0444);
MODULE_PARM_DESC(D0, "D0 GPIO line number");
module_param_named(D1, gpio_wiegand_d1, ushort, 0444);
MODULE_PARM_DESC(D1, "D1 GPIO line number");

MODULE_DESCRIPTION("Wiegand GPIO driver");
MODULE_AUTHOR("VerveWorks Pty. Ltd., updated for WB8 by Codex");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.2.0");

struct wiegand_state {
	u8 buffer[MAX_WIEGAND_BYTES];
	u8 last_buffer[MAX_WIEGAND_BYTES];
	int current_bit;
	int num_bits;
	int read_num;
	u64 last_ts;
	bool notify_pending;
};

static struct wiegand_state wiegand;
static struct hrtimer wiegand_timer;
static struct kobject *wiegand_kobj;
static struct work_struct notify_work;
static DEFINE_SPINLOCK(wiegand_lock);

static int irq_d0, irq_d1;

static int printbinary(char *buf, unsigned long x, int nbits)
{
	unsigned long mask = 1UL << (nbits - 1);

	while (mask != 0) {
		*buf++ = (mask & x) ? '1' : '0';
		mask >>= 1;
	}
	*buf = '\0';

	return nbits;
}

static void print_wiegand_data(char *output, u8 *buf, int nbits)
{
	int num_bytes = ((nbits - 1) / 8) + 1;
	int i;

	for (i = 0; i < num_bytes; ++i) {
		if (i == (num_bytes - 1)) {
			printbinary(output, buf[i] >> ((i + 1) * 8 - nbits),
				    nbits - i * 8);
			output += nbits - i * 8;
		} else {
			printbinary(output, buf[i], 8);
			output += 8;
		}
	}
}

static void wiegand_clear(struct wiegand_state *w)
{
	w->current_bit = 0;
	memset(w->buffer, 0, sizeof(w->buffer));
}

static void wiegand_init_state(struct wiegand_state *w)
{
	w->read_num = 0;
	wiegand_clear(w);
}

static void wiegand_notify_workfn(struct work_struct *work)
{
	sysfs_notify(wiegand_kobj, NULL, "read");
}

static ssize_t wiegand_show(struct kobject *kobj, struct kobj_attribute *attr,
			    char *buf)
{
	static char wiegand_buf[MAX_WIEGAND_BYTES * 8];
	unsigned long flags;
	int num_bits, read_num;

	spin_lock_irqsave(&wiegand_lock, flags);
	num_bits = wiegand.num_bits;
	read_num = wiegand.read_num;
	print_wiegand_data(wiegand_buf, wiegand.last_buffer, num_bits);
	spin_unlock_irqrestore(&wiegand_lock, flags);

	return sprintf(buf, "%.5d:%s\n", read_num, wiegand_buf);
}

static struct kobj_attribute wiegand_attribute =
	__ATTR(read, 0444, wiegand_show, NULL);

static struct attribute *attrs[] = {
	&wiegand_attribute.attr,
	NULL,
};

static struct attribute_group attr_group = {
	.attrs = attrs,
};

static enum hrtimer_restart wiegand_timer_fn(struct hrtimer *ltimer)
{
	unsigned long flags;
	struct wiegand_state *w = &wiegand;
	int num_bytes;
	u64 ts, interval;
	u64 missed;

	ts = ktime_get_ns();

	spin_lock_irqsave(&wiegand_lock, flags);
	interval = ts - w->last_ts;

	if (interval > FRAME_TIMEOUT_NS && w->current_bit != 0) {
		num_bytes = ((w->current_bit - 1) / 8) + 1;
		if ((w->current_bit % 4) == 0 && num_bytes <= MAX_WIEGAND_BYTES) {
			memcpy(w->last_buffer, w->buffer, num_bytes);
			w->num_bits = w->current_bit;
			w->read_num++;
			w->notify_pending = true;
		} else {
			pr_debug("wiegand-gpio: unexpected frame length [%d]\n",
				 w->current_bit);
		}
		wiegand_clear(w);
	}
	spin_unlock_irqrestore(&wiegand_lock, flags);

	missed = hrtimer_forward_now(&wiegand_timer,
				     ktime_set(0, TIMER_PERIOD_NS));
	if (missed > 1)
		pr_err("wiegand-gpio: missed ticks %llu\n", missed - 1);

	if (wiegand.notify_pending) {
		wiegand.notify_pending = false;
		schedule_work(&notify_work);
	}

	return HRTIMER_RESTART;
}

static irqreturn_t wiegand_data_isr(int irq, void *dev_id)
{
	struct wiegand_state *w = dev_id;
	unsigned long flags;
	u64 ts, interval;
	u8 value;

	ts = ktime_get_ns();
	spin_lock_irqsave(&wiegand_lock, flags);
	interval = ts - w->last_ts;
	w->last_ts = ts;

	if (interval < MIN_PULSE_INTERVAL_NS) {
		spin_unlock_irqrestore(&wiegand_lock, flags);
		return IRQ_HANDLED;
	}

	value = (irq == irq_d1) ? 0x80 : 0x00;
	if (w->current_bit < MAX_WIEGAND_BYTES * 8)
		w->buffer[w->current_bit / 8] |= (value >> (w->current_bit % 8));

	w->current_bit++;
	spin_unlock_irqrestore(&wiegand_lock, flags);

	return IRQ_HANDLED;
}

static void wiegand_free_irqs(void)
{
	if (irq_d0 > 0)
		free_irq(irq_d0, &wiegand);
	if (irq_d1 > 0)
		free_irq(irq_d1, &wiegand);
}

static void wiegand_free_gpios(void)
{
	gpio_free(gpio_wiegand_d0);
	gpio_free(gpio_wiegand_d1);
}

static int __init wiegand_init_module(void)
{
	int ret;

	pr_info("wiegand-gpio: initializing (D0=%hu, D1=%hu)\n",
		gpio_wiegand_d0, gpio_wiegand_d1);

	spin_lock_init(&wiegand_lock);
	wiegand_init_state(&wiegand);
	INIT_WORK(&notify_work, wiegand_notify_workfn);

	ret = gpio_request(gpio_wiegand_d0, "wiegand-d0");
	if (ret)
		return ret;

	ret = gpio_request(gpio_wiegand_d1, "wiegand-d1");
	if (ret)
		goto err_free_gpios;

	ret = gpio_direction_input(gpio_wiegand_d0);
	if (ret)
		goto err_free_gpios;

	ret = gpio_direction_input(gpio_wiegand_d1);
	if (ret)
		goto err_free_gpios;

	irq_d0 = gpio_to_irq(gpio_wiegand_d0);
	if (irq_d0 < 0) {
		pr_err("wiegand-gpio: can't request irq for D0 gpio\n");
		ret = irq_d0;
		goto err_free_gpios;
	}

	irq_d1 = gpio_to_irq(gpio_wiegand_d1);
	if (irq_d1 < 0) {
		pr_err("wiegand-gpio: can't request irq for D1 gpio\n");
		ret = irq_d1;
		goto err_free_gpios;
	}

	hrtimer_init(&wiegand_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	wiegand_timer.function = wiegand_timer_fn;
	hrtimer_start(&wiegand_timer, ktime_set(0, TIMER_PERIOD_NS),
		      HRTIMER_MODE_REL);

	ret = request_any_context_irq(irq_d0, wiegand_data_isr,
				      IRQF_SHARED | IRQF_TRIGGER_RISING,
				      "wiegand-data", &wiegand);
	if (ret) {
		pr_err("wiegand-gpio: can't register IRQ %d\n", irq_d0);
		goto err_timer;
	}

	ret = request_any_context_irq(irq_d1, wiegand_data_isr,
				      IRQF_SHARED | IRQF_TRIGGER_RISING,
				      "wiegand-data", &wiegand);
	if (ret) {
		pr_err("wiegand-gpio: can't register IRQ %d\n", irq_d1);
		goto err_irq;
	}

	wiegand_kobj = kobject_create_and_add("wiegand", kernel_kobj);
	if (!wiegand_kobj) {
		pr_err("wiegand-gpio: failed to create sysfs kobject\n");
		ret = -ENOMEM;
		goto err_irq2;
	}

	ret = sysfs_create_group(wiegand_kobj, &attr_group);
	if (ret) {
		kobject_put(wiegand_kobj);
		goto err_irq2;
	}

	pr_info("wiegand-gpio: ready\n");
	return 0;

err_irq2:
	wiegand_free_irqs();
err_irq:
	wiegand_free_irqs();
err_timer:
	hrtimer_cancel(&wiegand_timer);
err_free_gpios:
	wiegand_free_gpios();
	return ret;
}

static void __exit wiegand_cleanup_module(void)
{
	kobject_put(wiegand_kobj);
	hrtimer_cancel(&wiegand_timer);
	wiegand_free_irqs();
	wiegand_free_gpios();
	cancel_work_sync(&notify_work);

	pr_info("wiegand-gpio: removed\n");
}

module_init(wiegand_init_module);
module_exit(wiegand_cleanup_module);
