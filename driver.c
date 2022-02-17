// SPDX-License-Identifier: GPL-3.0-or-later

#include <linux/init.h>
#include <linux/module.h>

#include "init/my_sysfs.h"

#define DEFAULT_DEVICE_NAME "dev0"
#define DEFAULT_DEVICE_CAPACITY (2048*100)

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Leonid Geller");
MODULE_DESCRIPTION("A block device driver");
MODULE_VERSION("0.01");

/* Module parameter: 0 - auto device creation, 1 - user device creation */
static int mode;
module_param(mode, int, 0644);

/* Autocreated by module device */
struct block_dev *device;

/* Module initialization */
static int __init my_driver_init(void)
{
	int status;
	enum sysfs_registration_status sysr_status;

	pr_info("MYDRIVE: STARTING MODULE INITIALIZATION...\n");
	sysr_status = my_sysfs_init(mode);
	if (sysr_status != SYSFSREGISTRATON_OK) {
		pr_warn("MYDRIVE: (sysfs) %s\n",
			sysfs_registration_error_messages[sysr_status]);
		return -sysr_status;
	}
	pr_info("MYDRIVE: (sysfs) registered bus %s\n",
		my_bus_type.name);
	pr_info("MYDRIVE: (sysfs) registered driver %s on bus %s\n",
		mydriver.driver.name,
		my_bus_type.name);
	if (mode == 1)
		pr_info("MYDRIVE: (sysfs) added attributes command, input_command on driver %s\n",
		mydriver.driver.name);

	if (mode == 1) {
		pr_info("MYDRIVE: (mode) user device creation mode entered\n");
		return 0;
	}
	pr_info("MYDRIVE: (mode) auto device creation mode entered\n");

	status = my_device_create(&device,
				     DEFAULT_DEVICE_NAME,
				     DEFAULT_DEVICE_CAPACITY);
	if (status < 0) {
		pr_warn("MYDRIVE: (defaultdevice) default device init failed\n");
		my_sysfs_exit(mode);
		return status;
	}
	pr_info("MYDRIVE: (defaultdevice) default device created\n");

	status = my_device_register(device, DEFAULT_DEVICE_NAME);
	if (status) {
		pr_warn("MYDRIVE: (defaultdevice) device registration on bus failed\n");
		my_device_delete(device);
		my_sysfs_exit(mode);
		return -status;
	}
	pr_info("MYDRIVE: (defaultdevice) default device registered on bus\n");

	pr_info("MYDRIVE: Module initialized\n");
	return 0;
}

/* Module deinitialization */
static void __exit my_driver_exit(void)
{
	pr_info("MYDRIVE: EXITING MODULE...\n");
	if (mode == 1) {
		// clearing userdefined devices
		my_user_devices_unregister();
	} else if (device) {
		// clearing autocreated device
		my_device_unregister(device);
	}

	my_sysfs_exit(mode);
	pr_info("MYDRIVE: (sysfs) unregistered bus & driver\n");
	pr_info("MYDRIVE: Module removed\n");
}

module_init(my_driver_init);
module_exit(my_driver_exit);
