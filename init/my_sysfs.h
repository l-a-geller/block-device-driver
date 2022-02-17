/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once
#include "../device/my_device.h"

#define SYSFS_REGISTRATION_ERROR_MESSAGES_COUNT 5
#define BUS_NAME "mybus"
#define DRIVER_NAME "mydriver"
#define DEVICE_COMMAND_CREATE "create"
#define DEVICE_COMMAND_SETMODE "setmode"
#define DEVICE_COMMAND_LIST "Command list:\n" \
	"create device: create device_name device_capacity (in sectors)\n" \
	"set device mode: setmode device_name device_mode" \
		"(1 - read only, 0 - read & write)" \

struct user_device_list *user_device_list_head;

/* Bus configuration */
/* match devices to drivers (name test) */
static int my_match(struct device *dev, struct device_driver *driver)
{
	return !strncmp(dev_name(dev), driver->name, strlen(driver->name));
}

/* respond to hotplug user events */
static int my_uevent(struct device *dev, struct kobj_uevent_env *env)
{
	add_uevent_var(env, "DEV_NAME=%s", dev_name(dev));
	return 0;
}

struct bus_type my_bus_type = {
	.name   = BUS_NAME,
	.match  = my_match,
	.uevent = my_uevent,
};

/* Driver configuration */
struct my_driver {
	struct module *module;
	struct device_driver driver;
};

static struct my_driver mydriver = {
	.module = THIS_MODULE,
	.driver = {
		.name = DRIVER_NAME,
	},
};

int my_register_driver(struct my_driver *driver)
{
	int status;

	driver->driver.bus = &my_bus_type;
	status = driver_register(&driver->driver);
	if (status)
		return status;
	return 0;
}

void my_unregister_driver(struct my_driver *driver)
{
	driver_unregister(&driver->driver);
}

/* Registering & unregestering device */
int my_register_device(struct block_dev *mydev, char *dev_name)
{
	int res;

	mydev->dev.bus = &my_bus_type;
	mydev->dev.release = my_dev_release;
	dev_set_name(&mydev->dev, dev_name);
	res = device_register(&mydev->dev);
	return res;
}

void my_unregister_device(struct block_dev *mydev)
{
	pr_info("MYDRIVE: (device) unregistering device...\n");
	if (!mydev->gd)
		return;
	if (!mydev->gd->major || !mydev->gd->disk_name)
		return;
	unregister_blkdev(mydev->gd->major, mydev->gd->disk_name);
	pr_info("MYDRIVE: (device) default device unregistered\n");
	delete_block_device(mydev);
	device_unregister(&mydev->dev);
	kfree(mydev);
	pr_info("MYDRIVE: (device) default device removed\n");
}

void my_unregister_user_devices(void)
{
	while (user_device_list_head) {
		my_unregister_device(user_device_list_head->device);
		user_device_list_head = user_device_list_head->next;
	}
	list_destroy(user_device_list_head);
}

/* Driver attributes */
static ssize_t commands_show(struct device_driver *dev, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%s\n", DEVICE_COMMAND_LIST);
}

static ssize_t input_command_store(struct device_driver *dev,
				const char *buf,
				size_t count)
{
	char *command;
	char *device_name;
	int device_size;
	int status;
	struct block_dev *device;

	command = kmalloc_array(count, sizeof(char), GFP_KERNEL);
	device_name = kmalloc_array(count, sizeof(char), GFP_KERNEL);

	status = sscanf(buf, "%s %s %d", command, device_name, &device_size);
	if (status != 3) {
		pr_warn("MYDRIVE: (commands) couldn't recognise a command\n");
		goto out_free_parameter_buffers;
	}
	pr_info("MYDRIVE: (commands) command received: %s\n", command);

	if (!strncmp(command,
		     DEVICE_COMMAND_CREATE,
		     strlen(DEVICE_COMMAND_CREATE))) {
		pr_info("MYDRIVE: (commands) starting device creation (parameters: name = %s, capacity = %d)...\n",
			device_name,
			device_size);

		if (device_size <= 0) {
			pr_info("MYDRIVE_COMMANDS: (userdevice) device capacity (sectors count) should be a positive integer\n");
			goto out_free_parameter_buffers;
		}
		/* TODO: check on device_name uniqueness
		 * (kernel prints an error if device name repeats,
		 * but it needs to be made to look not that scary)
		 */

		status = create_block_device(&device, device_name, device_size);
		if (status < 0) {
			pr_warn("MYDRIVE: (userdevice) device init failed\n");
			goto out_free_parameter_buffers;
		}
		pr_info("MYDRIVE: (userdevice) device created");
		node_create(device);
		list_add_front(&user_device_list_head, device);
		status = my_register_device(device, device_name);
		if (status) {
			pr_warn("MYDRIVE: (userdevice) device registration on bus failed\n");
			goto out_free_parameter_buffers;
		}
		pr_info("MYDRIVE: (userdevice) device registered on bus");
	} else if (!strncmp(command,
			    DEVICE_COMMAND_SETMODE,
			    strlen(DEVICE_COMMAND_SETMODE))) {
		pr_info("MYDRIVE: (commands) starting device mode setting (parameters: name = %s, mode = %d)...\n",
			device_name,
			device_size);
		if (device_size < 0 || device_size > 1) {
			pr_warn("MYDRIVE: (setmode) mode must 1 - readonly or 0 - read & write\n");
			goto out_free_parameter_buffers;
		}
		device = list_search_name(user_device_list_head,
					  device_name)->device;
		if (!device) {
			pr_warn("MYDRIVE: (setmode) device with name %s not found\n",
				device_name);
			goto out_free_parameter_buffers;
		}
		device->mode = device_size;
		pr_info("MYDRIVE: (setmode) device %s mode set to %d\n",
			device_name,
			device_size);
	} else
		pr_info("MYDRIVE: (commands) command is not recognised. view list of available commands in commands attribute");

out_free_parameter_buffers:
	kfree(command);
	kfree(device_name);
	return count;
}
DRIVER_ATTR_RO(commands);
DRIVER_ATTR_WO(input_command);

enum sysfs_registration_status {
	SYSFSREGISTRATON_OK = 0,
	SYSFSREGISTRATON_BUS_FAILED,
	SYSFSREGISTRATON_DRIVER_FAILED,
	SYSFSREGISTRATON_DRIVER_AC_FAILED,
	SYSFSREGISTRATON_DRIVER_AI_FAILED
};

const char *sysfs_registration_error_messages
[SYSFS_REGISTRATION_ERROR_MESSAGES_COUNT] = {
	[SYSFSREGISTRATON_BUS_FAILED] =
		"bus registration failed",
	[SYSFSREGISTRATON_DRIVER_FAILED] =
		"driver registration on bus failed",
	[SYSFSREGISTRATON_DRIVER_AC_FAILED] =
		"driver commands attribute creation failed, driver initialized with no device usercreation",
	[SYSFSREGISTRATON_DRIVER_AI_FAILED] =
		"driver input_command attribute creation failed, driver initialized with no device usercreation"
};

/* Register bus, driver on a bus and driver attributes */
enum sysfs_registration_status my_sysfs_init(
	int is_user_device_registration_enabled)
{
	/* Registering a sysfs bus */
	int status;

	status = bus_register(&my_bus_type);
	if (status)
		return SYSFSREGISTRATON_BUS_FAILED;

	/* Registering a driver */
	status = my_register_driver(&mydriver);
	if (status) {
		bus_unregister(&my_bus_type);
		return SYSFSREGISTRATON_DRIVER_FAILED;
	}

	/* Adding driver attributes (
	 * commands - user views available commands,
	 * input_command - user inputs commands and receives results
	 * )
	 */
	if (is_user_device_registration_enabled) {
		status = driver_create_file(&(mydriver.driver),
					    &driver_attr_commands);
		if (status)
			return SYSFSREGISTRATON_DRIVER_AC_FAILED;
		status = driver_create_file(&(mydriver.driver),
					    &driver_attr_input_command);
		if (status)
			return SYSFSREGISTRATON_DRIVER_AI_FAILED;
	}
	return SYSFSREGISTRATON_OK;
}

void my_sysfs_exit(int is_user_device_registration_enabled)
{
	if (is_user_device_registration_enabled) {
		driver_remove_file(&(mydriver.driver),
				   &driver_attr_commands);
		driver_remove_file(&(mydriver.driver),
				   &driver_attr_input_command);
	}
	my_unregister_driver(&mydriver);
	bus_unregister(&my_bus_type);
}


