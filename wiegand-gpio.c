/* wiegand-gpio.c
 *
 * Wiegand driver using GPIO an interrupts.
 *
 */

/* Standard headers for LKMs */
#include <linux/module.h>	/* Needed by all modules */
#include <linux/kernel.h>	/* Needed for KERN_INFO */
#include <linux/kobject.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/timer.h>

#include <linux/tty.h>      /* console_print() interface */
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/interrupt.h>

#include <asm/irq.h>
#include <linux/gpio.h>

static ushort GPIO_WIEGAND_D0 = 0;
static ushort GPIO_WIEGAND_D1 = 0;

MODULE_DESCRIPTION("Wiegand GPIO driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("VerveWorks Pty. Ltd.");

module_param_named(D0, GPIO_WIEGAND_D0, ushort, S_IRUGO);
MODULE_PARM_DESC(D0, "D0 GPIO number");

module_param_named(D1, GPIO_WIEGAND_D1, ushort, S_IRUGO);
MODULE_PARM_DESC(D1, "D1 GPIO number");


#define MAX_WIEGAND_BYTES 6
#define MIN_PULSE_INTERVAL_USEC 700


static struct wiegand {
    char buffer[MAX_WIEGAND_BYTES];
    int currentBit;

    int readNum;
    char lastBuffer[MAX_WIEGAND_BYTES];
    int numBits;
} wiegand;


static struct timer_list timer;

static int printbinary(char *buf, unsigned long x, int nbits) {
	unsigned long mask = 1UL << (nbits - 1);
	while(mask != 0) {
		*buf++ = (mask & x ? '1' : '0');
		mask >>= 1;
	}
	*buf = '\0';

	return nbits;
}

void print_wiegand_data(char* output, char* buf, int nbits) {
    int numBytes = ((nbits -1) / 8 ) + 1;
    int i;

    for(i=0; i< numBytes; ++i) {
		if(i == (numBytes - 1)) {
		    printbinary(output, buf[i] >> ((i + 1) * 8 - nbits),  nbits - i * 8);
		    output += nbits - i * 8;
		} else {
			printbinary(output, buf[i], 8);
			output += 8;
		}
	}
}


static ssize_t wiegandShow(struct kobject *kobj, struct kobj_attribute *attr, char *buf) {
    static char wiegand_buf[MAX_WIEGAND_BYTES * 8];
	print_wiegand_data(wiegand_buf, wiegand.lastBuffer, wiegand.numBits);

    return sprintf(
        buf, "%.5d:%s\n",
        wiegand.readNum,
        wiegand_buf
    );
}

static struct kobj_attribute wiegand_attribute =
    __ATTR(read, S_IRUGO, wiegandShow, NULL);

static struct attribute *attrs[] = {
    &wiegand_attribute.attr,
    NULL,   /* need to NULL terminate the list of attributes */
};

static struct attribute_group attr_group = {
    .attrs = attrs,
};

static struct kobject *wiegandKObj;

irqreturn_t wiegand_data_isr(int irq, void *dev_id);

void wiegand_clear(struct wiegand *w) {
    w->currentBit = 0;
    memset(w->buffer, 0, MAX_WIEGAND_BYTES);
}

void wiegand_init(struct wiegand *w) {
    w->readNum = 0;
    wiegand_clear(w);
}

void wiegand_timer(unsigned long data) {
    char buf[MAX_WIEGAND_BYTES * 8];
    size_t i;
    struct wiegand *w = (struct wiegand *) data;
    int numBytes = ((w->currentBit -1) / 8 )+ 1;

    if(w->currentBit == 24 || w->currentBit == 26 || w->currentBit == 32 || w->currentBit == 34 || w->currentBit == 8 || w->currentBit == 4) {
        for(i=0; i< numBytes; ++i){
            w->lastBuffer[i] = w->buffer[i];
        }

        w->numBits = w->currentBit;
        w->readNum++;

        print_wiegand_data(buf, w->buffer, w->numBits);
        printk("wiegand-gpio: new read available [%d]: %s\n",  w->numBits, buf);

        sysfs_notify(wiegandKObj, NULL, "read");
    }

    //reset for next reading
    wiegand_clear(w);
}

static int irq_d0, irq_d1;

int init_module() {
    int retval, ret;

    printk("wiegand-gpio: intialising\n");

    wiegand_init(&wiegand);

    ret = gpio_request(GPIO_WIEGAND_D0, "wiegand-d0");
    if(ret) {
        return ret;
    }

    ret = gpio_request(GPIO_WIEGAND_D1, "wiegand-d1");
    if(ret) {
        return ret;
    }

    ret = gpio_direction_input(GPIO_WIEGAND_D0);
    if(ret) {
        return ret;
    }

    ret = gpio_direction_input(GPIO_WIEGAND_D1);
    if (ret) {
        return ret;
    }

	irq_d0 = gpio_to_irq(GPIO_WIEGAND_D0);
	if(irq_d0 < 0) {
		printk("wiegand-gpio: can't request irq for D0 gpio\n");
		return irq_d0;
	}

	irq_d1 = gpio_to_irq(GPIO_WIEGAND_D1);
	if(irq_d1 < 0) {
		printk("wiegand-gpio: can't request irq for D1 gpio\n");
		return irq_d1;
	}

	/** Request IRQ for pin */
    if(request_any_context_irq(irq_d0, wiegand_data_isr, IRQF_SHARED | IRQF_TRIGGER_FALLING, "wiegand-data", &wiegand)) {
        printk(KERN_DEBUG"wiegand-gpio: Can't register IRQ %d\n", irq_d0);
        return -EIO;
    }

    if(request_any_context_irq(irq_d1, wiegand_data_isr, IRQF_SHARED | IRQF_TRIGGER_FALLING, "wiegand-data", &wiegand)) {
        printk(KERN_DEBUG"wiegand-gpio: Can't register IRQ %d\n", irq_d1);
        return -EIO;
    }

    //setup the sysfs
    wiegandKObj = kobject_create_and_add("wiegand", kernel_kobj);

    if(!wiegandKObj) {
        printk("wiegand-gpio: failed to create sysfs\n");
        return -ENOMEM;
    }

    retval = sysfs_create_group(wiegandKObj, &attr_group);
    if(retval) {
        kobject_put(wiegandKObj);
    }

    //setup the timer
    init_timer(&timer);
    timer.function = wiegand_timer;
    timer.data = (unsigned long) &wiegand;

    printk("wiegand-gpio: ready\n");
    return retval;
}

irqreturn_t wiegand_data_isr(int irq, void *dev_id) {
    struct wiegand *w = (struct wiegand *)dev_id;
    struct timespec ts, interval;
    static struct timespec lastts;
    int value = (irq == irq_d1) ? 0x80 : 0;

    getnstimeofday(&ts);
    interval = timespec_sub(ts,lastts);
    lastts = ts;

    if((interval.tv_sec == 0 ) && (interval.tv_nsec < MIN_PULSE_INTERVAL_USEC * 1000)) {
        return IRQ_HANDLED;
    }

    del_timer(&timer);

    if(w->currentBit <=  MAX_WIEGAND_BYTES * 8) {
        w->buffer[(w->currentBit) / 8] |= (value >> ((w->currentBit) % 8));
    }

    w->currentBit++;

    //if we don't get another interrupt for 50ms we
    //assume the data is complete.
    timer.expires = jiffies + msecs_to_jiffies(50);
    add_timer(&timer);

    return IRQ_HANDLED;
}

void cleanup_module() {
    kobject_put(wiegandKObj);
    del_timer(&timer);

    free_irq(irq_d0, &wiegand);
    free_irq(irq_d1, &wiegand);

    gpio_free(GPIO_WIEGAND_D0);
    gpio_free(GPIO_WIEGAND_D1);

    printk("wiegand-gpio: removed\n");
}
