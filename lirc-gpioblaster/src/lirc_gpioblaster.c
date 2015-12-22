/*
 * lirc_gpioblaster.c
 *
 * lirc_gpioblaster - Device driver that sends IR codes using a GPIO pin
 *	      Code taken from the lirc_rpi module, receiver removed and
 *	      gpio management changed to be generic for any device.
 *	      so I would like say thanks to the authors.
 * 
 * Copyright (C) 2015 Daniel Gonzalez <dgcbueu@gmail.com>
 * Copyright (C) 2012 Aron Robert Szabo <aron@reon.hu>,
 *		      Michael Bishop <cleverca22@gmail.com>
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <linux/module.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/time.h>
#include <linux/timex.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/irq.h>
#include <linux/spinlock.h>
#include <media/lirc.h>
#include <media/lirc_dev.h>
#include <linux/gpio.h>

#define LIRC_DRIVER_NAME "lirc_rpi"
#define RBUF_LEN 256
#define LIRC_TRANSMITTER_LATENCY 50

#ifndef MAX_UDELAY_MS
#define MAX_UDELAY_US 5000
#else
#define MAX_UDELAY_US (MAX_UDELAY_MS*1000)
#endif

#define dprintk(fmt, args...)					\
	do {							\
		if (debug)					\
			printk(KERN_DEBUG LIRC_DRIVER_NAME ": "	\
			       fmt, ## args);			\
	} while (0)

/* module parameters */

/* set the default GPIO output pin */
static int gpio_out_pin = 0;
/* enable debugging messages */
static bool debug;
/* use softcarrier by default */
static bool softcarrier = 1;
/* 0 = do not invert output, 1 = invert output */
static bool invert = 0;

/* forward declarations */
static long send_pulse(unsigned long length);
static void send_space(long length);
static void lirc_rpi_exit(void);

static struct platform_device *lirc_rpi_dev;
static struct timeval lasttv = { 0, 0 };
static struct lirc_buffer rbuf;
static spinlock_t lock;

/* initialized/set in init_timing_params() */
static unsigned int freq = 38000;
static unsigned int duty_cycle = 50;
static unsigned long period;
static unsigned long pulse_width;
static unsigned long space_width;

static void safe_udelay(unsigned long usecs)
{
	while (usecs > MAX_UDELAY_US) {
		udelay(MAX_UDELAY_US);
		usecs -= MAX_UDELAY_US;
	}
	udelay(usecs);
}

static int init_timing_params(unsigned int new_duty_cycle,
	unsigned int new_freq)
{
	if (1000 * 1000000L / new_freq * new_duty_cycle / 100 <=
	    LIRC_TRANSMITTER_LATENCY)
		return -EINVAL;
	if (1000 * 1000000L / new_freq * (100 - new_duty_cycle) / 100 <=
	    LIRC_TRANSMITTER_LATENCY)
		return -EINVAL;
	duty_cycle = new_duty_cycle;
	freq = new_freq;
	period = 1000 * 1000000L / freq;
	pulse_width = period * duty_cycle / 100;
	space_width = period - pulse_width;
	dprintk("in init_timing_params, freq=%d pulse=%ld, "
		"space=%ld\n", freq, pulse_width, space_width);
	return 0;
}

static long send_pulse_softcarrier(unsigned long length)
{
	int flag;
	unsigned long actual, target;
	unsigned long actual_us, initial_us, target_us;

	length *= 1000;

	actual = 0; target = 0; flag = 0;
	read_current_timer(&actual_us);

	while (actual < length) {
		if (flag) {
			gpio_set_value(gpio_out_pin, invert);
			target += space_width;
		} else {
			gpio_set_value(gpio_out_pin, !invert);
			target += pulse_width;
		}
		initial_us = actual_us;
		target_us = actual_us + (target - actual) / 1000;
		/*
		 * Note - we've checked in ioctl that the pulse/space
		 * widths are big enough so that d is > 0
		 */
		if  ((int)(target_us - actual_us) > 0)
			udelay(target_us - actual_us);
		read_current_timer(&actual_us);
		actual += (actual_us - initial_us) * 1000;
		flag = !flag;
	}
	return (actual-length) / 1000;
}

static long send_pulse(unsigned long length)
{
	if (length <= 0)
		return 0;

	if (softcarrier) {
		return send_pulse_softcarrier(length);
	} else {
		gpio_set_value(gpio_out_pin, !invert);
		safe_udelay(length);
		return 0;
	}
}

static void send_space(long length)
{
	gpio_set_value(gpio_out_pin, invert);
	if (length <= 0)
		return;
	safe_udelay(length);
}

static int init_port(void)
{
	int ret;

	if (gpio_request(gpio_out_pin, LIRC_DRIVER_NAME " ir/out")) {
		printk(KERN_ALERT LIRC_DRIVER_NAME
		       ": cant claim gpio pin %d\n", gpio_out_pin);
		ret = -ENODEV;
		goto exit_init_port;
	}

	gpio_direction_output(gpio_out_pin, 1);
	gpio_set_value(gpio_out_pin, invert);

	exit_gpio_free_out_pin:
	gpio_free(gpio_out_pin);

	exit_init_port:
	return ret;
}

// called when the character device is opened
static int set_use_inc(void *data)
{
	return 0;
}

static void set_use_dec(void *data)
{
}

static ssize_t lirc_write(struct file *file, const char *buf,
	size_t n, loff_t *ppos)
{
	int i, count;
	unsigned long flags;
	long delta = 0;
	int *wbuf;

	count = n / sizeof(int);
	if (n % sizeof(int) || count % 2 == 0)
		return -EINVAL;
	wbuf = memdup_user(buf, n);
	if (IS_ERR(wbuf))
		return PTR_ERR(wbuf);
	spin_lock_irqsave(&lock, flags);

	for (i = 0; i < count; i++) {
		if (i%2)
			send_space(wbuf[i] - delta);
		else
			delta = send_pulse(wbuf[i]);
	}
	gpio_set_value(gpio_out_pin, invert);

	spin_unlock_irqrestore(&lock, flags);
	kfree(wbuf);
	return n;
}

static long lirc_ioctl(struct file *filep, unsigned int cmd, unsigned long arg)
{
	int result;
	__u32 value;

	switch (cmd) {
	case LIRC_GET_SEND_MODE:
		return -ENOIOCTLCMD;
		break;

	case LIRC_SET_SEND_MODE:
		result = get_user(value, (__u32 *) arg);
		if (result)
			return result;
		/* only LIRC_MODE_PULSE supported */
		if (value != LIRC_MODE_PULSE)
			return -ENOSYS;
		break;

	case LIRC_GET_LENGTH:
		return -ENOSYS;
		break;

	case LIRC_SET_SEND_DUTY_CYCLE:
		dprintk("SET_SEND_DUTY_CYCLE\n");
		result = get_user(value, (__u32 *) arg);
		if (result)
			return result;
		if (value <= 0 || value > 100)
			return -EINVAL;
		return init_timing_params(value, freq);
		break;

	case LIRC_SET_SEND_CARRIER:
		dprintk("SET_SEND_CARRIER\n");
		result = get_user(value, (__u32 *) arg);
		if (result)
			return result;
		if (value > 500000 || value < 20000)
			return -EINVAL;
		return init_timing_params(duty_cycle, value);
		break;

	default:
		return lirc_dev_fop_ioctl(filep, cmd, arg);
	}
	return 0;
}

static const struct file_operations lirc_fops = {
	.owner		= THIS_MODULE,
	.write		= lirc_write,
	.unlocked_ioctl	= lirc_ioctl,
	.read		= lirc_dev_fop_read,
	.poll		= lirc_dev_fop_poll,
	.open		= lirc_dev_fop_open,
	.release	= lirc_dev_fop_close,
	.llseek		= no_llseek,
};

static struct lirc_driver driver = {
	.name		= LIRC_DRIVER_NAME,
	.minor		= -1,
	.code_length	= 1,
	.sample_rate	= 0,
	.data		= NULL,
	.add_to_buf	= NULL,
	.rbuf		= &rbuf,
	.set_use_inc	= set_use_inc,
	.set_use_dec	= set_use_dec,
	.fops		= &lirc_fops,
	.dev		= NULL,
	.owner		= THIS_MODULE,
};

static struct platform_driver lirc_rpi_driver = {
	.driver = {
		.name   = LIRC_DRIVER_NAME,
		.owner  = THIS_MODULE,
	},
};

static int __init lirc_rpi_init(void)
{
	int result;

	/* Init read buffer. */
	result = lirc_buffer_init(&rbuf, sizeof(int), RBUF_LEN);
	if (result < 0)
		return -ENOMEM;

	result = platform_driver_register(&lirc_rpi_driver);
	if (result) {
		printk(KERN_ERR LIRC_DRIVER_NAME
		       ": lirc register returned %d\n", result);
		goto exit_buffer_free;
	}

	lirc_rpi_dev = platform_device_alloc(LIRC_DRIVER_NAME, 0);
	if (!lirc_rpi_dev) {
		result = -ENOMEM;
		goto exit_driver_unregister;
	}

	result = platform_device_add(lirc_rpi_dev);
	if (result)
		goto exit_device_put;

	return 0;

	exit_device_put:
	platform_device_put(lirc_rpi_dev);

	exit_driver_unregister:
	platform_driver_unregister(&lirc_rpi_driver);

	exit_buffer_free:
	lirc_buffer_free(&rbuf);

	return result;
}

static void lirc_rpi_exit(void)
{
	platform_device_unregister(lirc_rpi_dev);
	platform_driver_unregister(&lirc_rpi_driver);
	lirc_buffer_free(&rbuf);
}

static int __init lirc_rpi_init_module(void)
{
	int result;

	result = lirc_rpi_init();
	if (result)
		return result;

	result = init_port();
	if (result < 0)
		goto exit_rpi;

	driver.features = LIRC_CAN_SET_SEND_DUTY_CYCLE |
			  LIRC_CAN_SET_SEND_CARRIER |
			  LIRC_CAN_SEND_PULSE;

	driver.dev = &lirc_rpi_dev->dev;
	driver.minor = lirc_register_driver(&driver);

	if (driver.minor < 0) {
		printk(KERN_ERR LIRC_DRIVER_NAME
		       ": device registration failed with %d\n", result);
		result = -EIO;
		goto exit_rpi;
	}

	printk(KERN_INFO LIRC_DRIVER_NAME ": driver registered!\n");

	return 0;

	exit_rpi:
	lirc_rpi_exit();

	return result;
}

static void __exit lirc_rpi_exit_module(void)
{
	gpio_free(gpio_out_pin);

	lirc_rpi_exit();

	lirc_unregister_driver(driver.minor);
	printk(KERN_INFO LIRC_DRIVER_NAME ": cleaned up module\n");
}

module_init(lirc_rpi_init_module);
module_exit(lirc_rpi_exit_module);

MODULE_DESCRIPTION("Infra-red gpio blaster driver for any device.");
MODULE_AUTHOR("Aron Robert Szabo <aron@reon.hu>");
MODULE_AUTHOR("Michael Bishop <cleverca22@gmail.com>");
MODULE_AUTHOR("Daniel Gonzalez <dgcbueu@gmail.com>");
MODULE_LICENSE("GPL");

module_param(gpio_out_pin, int, S_IRUGO);
MODULE_PARM_DESC(gpio_out_pin, "GPIO output/transmitter pin number of the BCM"
		 "default 17");

module_param(softcarrier, bool, S_IRUGO);
MODULE_PARM_DESC(softcarrier, "Software carrier (0 = off, 1 = on, default on)");

module_param(invert, bool, S_IRUGO);
MODULE_PARM_DESC(invert, "Invert output (0 = off, 1 = on, default off");

module_param(debug, bool, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(debug, "Enable debugging messages");
