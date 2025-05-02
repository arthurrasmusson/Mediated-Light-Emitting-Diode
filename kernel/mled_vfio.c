// SPDX-License-Identifier: GPL-2.0
/*
 * mled-vfio.ko ─ a **minimal** mediated-device that exposes one writable byte
 * to a guest VM; the byte toggles a real GPIO LED on the host.
 *
 *   guest writes 0 → LED off
 *   guest writes 1 → LED on
 *
 * 2025-05-02  Arthur Rasmusson
 *
 * ---------------------------------------------------------------------------
 * Kernel-API compatibility
 * ---------------------------------------------------------------------------
 * For Linux < 6.5:
 *   vfio_alloc_device(TYPE, device, group, ops)
 *
 * For Linux >= 6.5 (and later, including 6.8):
 *   vfio_alloc_device(dev_struct, member, dev, ops)
 * where 'member' is the name of the embedded struct vfio_device field.
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/version.h>
#include <linux/mdev.h>
#include <linux/vfio.h>
#include <linux/gpio/consumer.h>
#include <linux/mutex.h>

#define DRV_NAME   "mled-vfio"
#define CLASS_NAME "vfio-gpio-led"

/* We emulate a PCI BAR0 that is exactly one byte long */
#define REGION_IDX_DATA VFIO_PCI_BAR0_REGION_INDEX
#define REGION_SIZE     1

// Additional note: This minimal "BAR0" region is used by the guest to read/write
// a single byte that controls the LED on the host.

/* -------------------------------------------------------------------------- */
/*               Version-dependent glue                                       */
/* -------------------------------------------------------------------------- */

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 5, 0)
/* ---------------------------- < 6.5 --------------------------------------- */
#define HAVE_IOMMUFD_OPS 0
#define mdev_to_dev(m) (&(m)->dev)
/*
 * Old macro signature:
 *   vfio_alloc_device(TYPE, device, group, ops)
 *
 * We pass NULL for group because mdev sets it up automatically.
 */
#define VFIO_ALLOC(state_ptr, mdev)                                            \
	do {                                                                   \
		state_ptr = vfio_alloc_device(struct led_mdev_state,           \
					      mdev_to_dev(mdev),               \
					      NULL,                            \
					      &led_ops);                       \
	} while (0)

#else
/* ---------------------------- >= 6.5 -------------------------------------- */
#define HAVE_IOMMUFD_OPS 1
#include <linux/iommufd.h>  /* pulls in the iommufd-based helpers */
#define mdev_to_dev(m) mdev_dev(m)
/*
 * New macro signature (4 args):
 *   vfio_alloc_device(dev_struct, member, dev, ops)
 *
 *  - dev_struct = the name of your struct (NO "struct" keyword),
 *  - member     = the name of the embedded struct vfio_device field,
 *  - dev        = parent device,
 *  - ops        = pointer to vfio_device_ops
 */
#define VFIO_ALLOC(state_ptr, mdev)                                            \
	do {                                                                   \
		state_ptr = vfio_alloc_device(led_mdev_state, vdev,           \
					      mdev_to_dev(mdev),               \
					      &led_ops);                       \
	} while (0)

#endif

/* -------------------------------------------------------------------------- */
/*               Per-device state                                             */
/* -------------------------------------------------------------------------- */

/**
 * struct led_mdev_state - our private device context
 * @vdev:   embedded VFIO device (must be first for container_of())
 * @mdev:   parent mediated device pointer
 * @led_gpio: handle to an actual GPIO line
 * @value:  the last byte written by the guest (0 or 1)
 * @lock:   protects @value and the LED GPIO
 */
// Additional note: The 'vdev' is the actual VFIO device object. The rest of
// the fields handle our LED-specific state tracking and concurrency.
struct led_mdev_state {
	/* Must be first, so container_of(&vdev, led_mdev_state, vdev) works */
	struct vfio_device  vdev;
	struct mdev_device *mdev;

	struct gpio_desc   *led_gpio;
	u8                  value;

	struct mutex        lock; /* protects value + GPIO */
};

// Additional note: This helper simply casts from the embedded vfio_device to
// our 'led_mdev_state' struct. We rely on the 'vdev' field being at offset zero.
static inline struct led_mdev_state *
vdev_to_state(struct vfio_device *v)
{
	return container_of(v, struct led_mdev_state, vdev);
}

/* -------------------------------------------------------------------------- */
/*               Hardware helper                                              */
/* -------------------------------------------------------------------------- */

// Additional note: This function updates the host GPIO line to reflect the
// cached LED value that the guest last wrote. A typical use case is turning
// a physical LED on or off based on 's->value'.
static void led_hw_update(struct led_mdev_state *s)
{
	/* Caller holds s->lock */
	if (s->led_gpio)
		gpiod_set_value_cansleep(s->led_gpio, !!s->value);
}

/* -------------------------------------------------------------------------- */
/*               VFIO data-plane callbacks                                    */
/* -------------------------------------------------------------------------- */

// Additional note: The read/write/ioctl callbacks are invoked when QEMU
// or other VFIO userspace does read/write/ioctl on the device FD. They
// implement minimal PCI-like behavior: a 1-byte region.

static ssize_t led_read(struct vfio_device *v,
			char __user *buf, size_t count, loff_t *ppos)
{
	// Additional note: We only provide 1 byte at offset 0. Once that is
	// read, subsequent reads return 0 bytes. This simulates a small
	// read-only device region.

	if (*ppos >= REGION_SIZE || !count)
		return 0;

	if (copy_to_user(buf, &vdev_to_state(v)->value, 1))
		return -EFAULT;

	*ppos = 1;
	return 1;
}

static ssize_t led_write(struct vfio_device *v,
			 const char __user *buf, size_t count, loff_t *ppos)
{
	struct led_mdev_state *s = vdev_to_state(v);
	u8 val;

	// Additional note: Writes must occur at offset 0, and we only accept
	// 1 byte. We then store it in 'value' and apply it to the LED.

	if (*ppos || !count)
		return -EINVAL;

	if (copy_from_user(&val, buf, 1))
		return -EFAULT;

	mutex_lock(&s->lock);
	s->value = val ? 1 : 0;
	led_hw_update(s);
	mutex_unlock(&s->lock);

	*ppos = 1;
	return 1;
}

static int led_region_info(struct vfio_region_info *ri)
{
	// Additional note: Userspace (QEMU, etc.) calls GET_REGION_INFO to
	// discover how large each region is and what flags it has.

	if (ri->index != REGION_IDX_DATA)
		return -EINVAL;

	ri->offset = 0;
	ri->size   = REGION_SIZE;
	ri->flags  = VFIO_REGION_INFO_FLAG_READ |
		     VFIO_REGION_INFO_FLAG_WRITE;
	return 0;
}

static long led_ioctl(struct vfio_device *v,
		      unsigned int cmd, unsigned long arg)
{
	unsigned long min;

	// Additional note: The only VFIO ioctls we handle are GET_INFO and
	// GET_REGION_INFO. Others return ENOTTY. This is enough for a simple
	// device that exposes only a single memory region without interrupts.

	switch (cmd) {
	case VFIO_DEVICE_GET_INFO: {
		struct vfio_device_info info;

		min = offsetofend(struct vfio_device_info, num_irqs);
		if (copy_from_user(&info, (void __user *)arg, min))
			return -EFAULT;
		if (info.argsz < min)
			return -EINVAL;

		/* Fake a "PCI" device so QEMU sees a PCI region. */
		info.flags       = VFIO_DEVICE_FLAGS_PCI;
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
/*               VFIO lifecycle callbacks                                    */
/* -------------------------------------------------------------------------- */

// Additional note: The .init and .release callbacks below correspond to the
// device lifecycle for a VFIO/mdev device. The LED is requested in ->init
// and freed in ->release. Typically, these get triggered when VFIO sets up
// or tears down the device for use by a guest.

static int led_init(struct vfio_device *v)
{
	struct led_mdev_state *s = vdev_to_state(v);

	/* Acquire a GPIO named "status" (or whatever name your DT/ACPI uses). */
	s->led_gpio = gpiod_get(mdev_to_dev(s->mdev), "status", GPIOD_OUT_LOW);
	if (IS_ERR(s->led_gpio))
		return PTR_ERR(s->led_gpio);

	mutex_init(&s->lock);
	return 0;
}

static void led_release(struct vfio_device *v)
{
	struct led_mdev_state *s = vdev_to_state(v);

	if (!IS_ERR_OR_NULL(s->led_gpio))
		gpiod_put(s->led_gpio);
}

/* -------------------------------------------------------------------------- */
/*               VFIO operations table                                        */
/* -------------------------------------------------------------------------- */

// Additional note: This table defines our VFIO device's operations, which
// includes init/release, read/write, and optionally iommufd-based attach/detach.

static const struct vfio_device_ops led_ops = {
	.name    = "vfio-gpio-led",
	.init    = led_init,
	.release = led_release,
	.read    = led_read,
	.write   = led_write,
	.ioctl   = led_ioctl,

#if HAVE_IOMMUFD_OPS
	.bind_iommufd   = vfio_iommufd_emulated_bind,
	.unbind_iommufd = vfio_iommufd_emulated_unbind,
	.attach_ioas    = vfio_iommufd_emulated_attach_ioas,
	.detach_ioas    = vfio_iommufd_emulated_detach_ioas,
#endif
};

/* -------------------------------------------------------------------------- */
/*               mdev glue (one type, many instances)                         */
/* -------------------------------------------------------------------------- */

// Additional note: This code registers a "parent" that can create multiple
// mediated devices of type "led-1", each hooking into the same driver.

static struct mdev_parent parent;
static struct mdev_type led_type = {
	.sysfs_name  = "led-1",
	.pretty_name = "gpio-led",
};

/**
 * led_probe() - called when userspace creates a new mediated device
 */
static int led_probe(struct mdev_device *mdev)
{
	struct led_mdev_state *s;

	/* The VFIO_ALLOC() macro picks the right vfio_alloc_device() usage */
	VFIO_ALLOC(s, mdev);
	if (IS_ERR(s))
		return PTR_ERR(s);

	s->mdev = mdev;
	dev_set_drvdata(&mdev->dev, s);

	/* For iommufd-based drivers, we call vfio_register_emulated_iommu_dev() */
	return vfio_register_emulated_iommu_dev(&s->vdev);
}

/**
 * led_remove() - called when userspace destroys the mediated device
 */
static void led_remove(struct mdev_device *mdev)
{
	struct led_mdev_state *s = dev_get_drvdata(&mdev->dev);

	vfio_unregister_group_dev(&s->vdev);
	vfio_put_device(&s->vdev);
}

/**
 * led_avail() - returns how many more mdev instances can be created
 * @t: the mdev_type
 *
 * Return: a static limit of 16
 */
// Additional note: This function is polled by the mdev core to see if we can
// still create additional devices of this type. Here we set an arbitrary max.
static unsigned int led_avail(struct mdev_type *t)
{
	return 16; /* Allow up to 16 concurrent mdev instances */
}

static struct mdev_driver led_driver = {
	.device_api = VFIO_DEVICE_API_PCI_STRING,
	.driver = {
		.name  = DRV_NAME,
		.owner = THIS_MODULE,
	},
	.probe         = led_probe,
	.remove        = led_remove,
	.get_available = led_avail,
};

/* -------------------------------------------------------------------------- */
/*               Module boilerplate                                          */
/* -------------------------------------------------------------------------- */

// Additional note: In typical kernel modules, we register the driver in
// module_init() and unregister in module_exit().

/**
 * led_init_mod() - module init function
 * Registers the mdev_driver and its single mdev_type
 *
 * Return: 0 on success or a negative error code.
 */
// More explanation: We first register the mdev_driver, then the parent that
// supports the "led_type". If anything fails, we unwind carefully.
static int __init led_init_mod(void)
{
	int ret;

	ret = mdev_register_driver(&led_driver);
	if (ret)
		return ret;

	ret = mdev_register_parent(&parent, NULL, &led_driver,
				   (struct mdev_type *[]) { &led_type }, 1);
	if (ret)
		mdev_unregister_driver(&led_driver);

	return ret;
}

/**
 * led_exit_mod() - module exit function
 * Unregisters everything we registered in led_init_mod().
 */
// More explanation: This removes the parent and the driver from the system,
// ensuring no leftover resources remain after the module is removed.
static void __exit led_exit_mod(void)
{
	mdev_unregister_parent(&parent);
	mdev_unregister_driver(&led_driver);
}

module_init(led_init_mod);
module_exit(led_exit_mod);

MODULE_AUTHOR("Arthur Rasmusson");
MODULE_DESCRIPTION("Sample VFIO-mdev GPIO LED");
MODULE_LICENSE("GPL");

