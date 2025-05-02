// SPDX-License-Identifier: GPL-2.0
/*
 * mled-vfio.ko
 *
 * VFIO-mdev sample that exposes ONE writable byte to a guest.
 *   0 → LED off
 *   1 → LED on
 *
 * On the host a real Raspberry-Pi GPIO is toggled via libgpiod.
 * This is the minimal skeleton you need to glue Neo-Jia style
 * mdev plumbing onto any arbitrary low-level backend.
 *
 * 2025-05-02  Arthur Rasmusson
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/mdev.h>
#include <linux/vfio.h>
#include <linux/gpio/consumer.h>
#include <linux/mutex.h>

#define DRV_NAME          "mled-vfio"
#define CLASS_NAME        "vfio-gpio-led"

/* We fake a “PCI function” with exactly one region (BAR0). */
#define REGION_IDX_DATA   VFIO_PCI_BAR0_REGION_INDEX
#define REGION_SIZE       1         /* single byte exposed to the guest */

/* -------------------------------------------------------------------------- */
/* Per-mediated-device state */
struct led_mdev_state {
	struct vfio_device  vdev;       /* first → container_of trick        */
	struct mdev_device *mdev;       /* pointer back to mediated device   */

	struct gpio_desc   *led_gpio;   /* handle returned by gpiod_get()    */
	u8                  value;      /* last byte written by the guest    */

	struct mutex        lock;       /* protects value + GPIO             */
};

static struct led_mdev_state *vdev_to_state(struct vfio_device *v)
{
	return container_of(v, struct led_mdev_state, vdev);
}

/* -------------------------------------------------------------------------- */
/* Helper: push cached ‘value’ out to real GPIO */

static void led_hw_update(struct led_mdev_state *s)
{
	/* Must hold s->lock */
	if (s->led_gpio)
		gpiod_set_value_cansleep(s->led_gpio, !!s->value);
}

/* -------------------------------------------------------------------------- */
/* VFIO data-plane callbacks */

static ssize_t led_read(struct vfio_device *vdev,
		        char __user *buf, size_t count, loff_t *ppos)
{
	/* region is only 1 byte long → EOF after first read            */
	if (*ppos >= REGION_SIZE || !count)
		return 0;

	if (copy_to_user(buf, &vdev_to_state(vdev)->value, 1))
		return -EFAULT;
	*ppos = 1;
	return 1;
}

static ssize_t led_write(struct vfio_device *vdev,
		         const char __user *buf, size_t count, loff_t *ppos)
{
	struct led_mdev_state *s = vdev_to_state(vdev);
	u8 val;

	/* Only allow a single-byte write at offset 0                   */
	if (*ppos || !count)
		return -EINVAL;
	if (copy_from_user(&val, buf, 1))
		return -EFAULT;

	mutex_lock(&s->lock);
	s->value = val ? 1 : 0; /* coerce to 0/1                        */
	led_hw_update(s);       /* actually toggle the pin              */
	mutex_unlock(&s->lock);

	*ppos = 1;
	return 1;
}

/* Return region layout for GET_REGION_INFO */
static int led_region_info(struct vfio_region_info *ri)
{
	if (ri->index != REGION_IDX_DATA)
		return -EINVAL;

	ri->offset = 0;
	ri->size   = REGION_SIZE;
	ri->flags  = VFIO_REGION_INFO_FLAG_READ |
	             VFIO_REGION_INFO_FLAG_WRITE;
	return 0;
}

/* All other VFIO ioctls funnel through here.  We only implement the
 * bare minimum: device info + region info. */
static long led_ioctl(struct vfio_device *v,
                      unsigned int cmd, unsigned long arg)
{
	unsigned long min;
	switch (cmd) {
	case VFIO_DEVICE_GET_INFO: {
		struct vfio_device_info info;
		min = offsetofend(struct vfio_device_info, num_irqs);
		if (copy_from_user(&info, (void __user *)arg, min))
			return -EFAULT;
		if (info.argsz < min)
			return -EINVAL;

		info.flags       = VFIO_DEVICE_FLAGS_PCI;  /* fake PCI */
		info.num_regions = REGION_IDX_DATA + 1;
		info.num_irqs    = 0;

		return copy_to_user((void __user *)arg, &info, min)
		       ? -EFAULT : 0;
	}
	case VFIO_DEVICE_GET_REGION_INFO: {
		struct vfio_region_info ri;
		min = offsetofend(struct vfio_region_info, offset);
		if (copy_from_user(&ri, (void __user *)arg, min))
			return -EFAULT;
		if (ri.argsz < min)
			return -EINVAL;

		if (led_region_info(&ri))
			return -EINVAL;

		return copy_to_user((void __user *)arg, &ri, min)
		       ? -EFAULT : 0;
	}
	default:
		return -ENOTTY;
	}
}

/* -------------------------------------------------------------------------- */
/* Lifecycle hooks */

static int led_init(struct vfio_device *v)
{
	struct led_mdev_state *s = vdev_to_state(v);

	/* GPIO is looked up by a DT alias/ACPI handle named “status”.
	 * Change the second arg to force a different pin. */
	s->led_gpio = gpiod_get(&mdev_dev(s->mdev)->dev,
	                        "status", GPIOD_OUT_LOW);
	if (IS_ERR(s->led_gpio))
		return PTR_ERR(s->led_gpio);

	mutex_init(&s->lock);
	return 0;
}

static void led_release(struct vfio_device *v)
{
	struct led_mdev_state *s = vdev_to_state(v);

	if (s->led_gpio)
		gpiod_put(s->led_gpio);
}

/* -------------------------------------------------------------------------- */
/* VFIO dispatch table (minimal) */

static const struct vfio_device_ops led_ops = {
	.name           = "vfio-gpio-led",
	.init           = led_init,
	.release        = led_release,
	.read           = led_read,
	.write          = led_write,
	.ioctl          = led_ioctl,

	/* All IOMMU-FD helpers route to the simple “emulated” variant */
	.bind_iommufd	= vfio_iommufd_emulated_bind,
	.unbind_iommufd	= vfio_iommufd_emulated_unbind,
	.attach_ioas	= vfio_iommufd_emulated_attach_ioas,
	.detach_ioas	= vfio_iommufd_emulated_detach_ioas,
};

/* -------------------------------------------------------------------------- */
/* mdev glue: one type (“led-1”), unlimited instances */

static struct mdev_parent parent;
static struct mdev_type led_type = {
	.sysfs_name  = "led-1",
	.pretty_name = "gpio-led",
};

/* Called whenever `echo <uuid> > …/led-1/create` happens */
static int led_probe(struct mdev_device *mdev)
{
	struct led_mdev_state *s;

	/* allocate & register vfio_device wrapper */
	s = vfio_alloc_device(s, vdev, &mdev->dev, &led_ops);
	if (IS_ERR(s))
		return PTR_ERR(s);

	s->mdev = mdev;
	dev_set_drvdata(&mdev->dev, s);

	return vfio_register_emulated_iommu_dev(&s->vdev);
}

/* Cleanup when that mediated device is destroyed */
static void led_remove(struct mdev_device *mdev)
{
	struct led_mdev_state *s = dev_get_drvdata(&mdev->dev);

	vfio_unregister_group_dev(&s->vdev);
	vfio_put_device(&s->vdev);
}

/* Advertise how many instances we can still create (arbitrary 16) */
static unsigned int led_avail(struct mdev_type *t) { return 16; }

static struct mdev_driver led_driver = {
	.device_api  = VFIO_DEVICE_API_PCI_STRING,
	.driver = {
		.name  = DRV_NAME,
		.owner = THIS_MODULE,
	},
	.probe         = led_probe,
	.remove        = led_remove,
	.get_available = led_avail,
};

/* -------------------------------------------------------------------------- */
/* Module entry points */

static int __init led_init_mod(void)
{
	int ret;

	/* Register with mdev core first */
	ret = mdev_register_driver(&led_driver);
	if (ret)
		return ret;

	/* Create the parent device so sysfs exposes led-1/create */
	ret = mdev_register_parent(&parent, NULL, &led_driver,
	                           (struct mdev_type *[]){ &led_type }, 1);
	if (ret)
		mdev_unregister_driver(&led_driver);
	return ret;
}

static void __exit led_exit_mod(void)
{
	mdev_unregister_parent(&parent);
	mdev_unregister_driver(&led_driver);
}

module_init(led_init_mod);
module_exit(led_exit_mod);

MODULE_AUTHOR("Arthur Rasmusson");
MODULE_DESCRIPTION("VFIO mdev GPIO LED");
MODULE_LICENSE("GPL");

