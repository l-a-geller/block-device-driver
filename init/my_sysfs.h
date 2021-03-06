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

int my_driver_register(struct my_driver *driver)
{
	int status;

	if (!driver)
		return -1;
	driver->driver.bus = &my_bus_type;
	status = driver_register(&driver->driver);
	if (status)
		return status;
	return 0;
}

void my_driver_unregister(struct my_driver *driver)
{
	if (!driver)
		return;
	driver_unregister(&driver->driver);
}

/* Registering & unregestering device */
int my_device_register(struct block_dev *mydev, char *dev_name)
{
	int res;

	if (!mydev || !dev_name)
		return -1;
	mydev->dev.bus = &my_bus_type;
	mydev->dev.release = my_device_release_from_bus;
	dev_set_name(&mydev->dev, dev_name);
	res = device_register(&mydev->dev);
	return res;
}

void my_device_unregister(struct block_dev *mydev)
{
	if (!mydev || !mydev->gd)
		return;
	if (!mydev->gd->major || !mydev->gd->disk_name)
		return;
	/* Releasing major */
	unregister_blkdev(mydev->gd->major, mydev->gd->disk_name);

	/* Clearing device internals (gd, queue, data) */
	my_device_delete(mydev);

	/* Unregistering device from a bus */
	device_unregister(&mydev->dev);

	kfree(mydev);
}

void my_user_devices_unregister(void)
{
	while (user_device_list_head) {
		my_device_unregister(user_device_list_head->device);
		user_device_list_head = user_device_list_head->next;
	}

	list_destroy(user_device_list_head);
}

/* Driver attributes */
static ssize_t commands_show(struct device_driver *dev, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%s\n", DEVICE_COMMAND_LIST);
}

static int user_device_create(char *device_name, int device_size)
{
	int status;
	struct block_dev *device;

	pr_info("MYDRIVE: (commands) starting device creation (parameters: name = %s, capacity = %d)...\n",
		device_name,
		device_size);

	if (device_size <= 0) {
		pr_warn("MYDRIVE: (userdevice) device capacity (sectors count) should be a positive integer\n");
		return -1;
	}

	status = list_check_unique_name(user_device_list_head, device_name);
	if (status < 0) {
		pr_warn("MYDRIVE: (userdevice) device with name %s already exists, choose another name\n",
			device_name);
		return -2;
	}
	/* TODO: check on device_name uniqueness
	 * (kernel prints an error if device name repeats,
	 * but it needs to be made to look not that scary)
	 */

	status = my_device_create(&device, device_name, device_size);
	if (status < 0) {
		pr_warn("MYDRIVE: (userdevice) device init failed\n");
		return -3;
	}
	pr_info("MYDRIVE: (userdevice) device created\n");

	status = my_device_register(device, device_name);
	if (status) {
		pr_warn("MYDRIVE: (userdevice) device registration on bus failed\n");
		my_device_delete(device);
		return -4;
	}

	if (!node_create(device)) {
		pr_warn("MYDRIVE: (userdevice) device adding to list of user devices failed\n");
		my_device_unregister(device);
		my_device_delete(device);
		return -5;
	}
	list_add_front(&user_device_list_head, device);

	pr_info("MYDRIVE: (userdevice) device registered on bus\n");
	return 0;
}

static int user_device_setmode(char *device_name, int device_mode)
{
	struct user_device_list *usr_dev;
	struct block_dev *device;

	pr_info("MYDRIVE: (commands) starting device mode setting (parameters: name = %s, mode = %d)...\n",
		device_name,
		device_mode);

	if (!user_device_list_head) {
		pr_warn("MYDRIVE: (setmode) no devices created yet, setmode failed\n");
		return -1;
	}

	if (device_mode < 0 || device_mode > 1) {
		pr_warn("MYDRIVE: (setmode) mode must 1 - readonly or 0 - read & write\n");
		return -2;
	}

	usr_dev = list_search_name(user_device_list_head,
				   device_name);
	if (!usr_dev || !usr_dev->device) {
		pr_warn("MYDRIVE: (setmode) device with name %s not found\n",
			device_name);
		return -3;
	}
	device = usr_dev->device;
	device->mode = device_mode;
	pr_info("MYDRIVE: (setmode) device %s mode set to %d\n",
		device_name,
		device_mode);
	return 0;
}

static void command_find(char *command, char *device_name, int argument)
{
	int status;

	if (!strncmp(command,
		     DEVICE_COMMAND_CREATE,
		     strlen(DEVICE_COMMAND_CREATE))) {
		status = user_device_create(device_name, argument);
		if (status < 0)
			pr_warn("MYDRIVE: (commands) error on create\n");
	} else if (!strncmp(command,
			    DEVICE_COMMAND_SETMODE,
			    strlen(DEVICE_COMMAND_SETMODE))) {
		status = user_device_setmode(device_name, argument);
		if (status < 0)
			pr_warn("MYDRIVE: (commands) error on setmode\n");
	} else {
		pr_info("MYDRIVE: (commands) command is not recognised. view list of available commands in commands attribute\n");
	}
}

static ssize_t input_command_store(struct device_driver *dev,
				const char *buf,
				size_t count)
{
	char *command;
	char *device_name;
	int argument;
	int status;

	command = kmalloc_array(count, sizeof(char), GFP_KERNEL);
	if (!command)
		return count;
	device_name = kmalloc_array(count, sizeof(char), GFP_KERNEL);
	if (!device_name) {
		kfree(command);
		pr_warn("MYDRIVE: (commands) couldn't allocate device_name, input probably is too long\n");
		return count;
	}

	status = sscanf(buf, "%s %s %d", command, device_name, &argument);
	if (status != 3) {
		pr_warn("MYDRIVE: (commands) couldn't recognise a command\n");
		goto out_free_parameter_buffers;
	}
	pr_info("MYDRIVE: (commands) command received: %s\n", command);

	command_find(command, device_name, argument);

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
		"driver commands attribute creation failed",
	[SYSFSREGISTRATON_DRIVER_AI_FAILED] =
		"driver input_command attribute creation failed"
};

/* Register bus, driver on a bus and driver attributes */
enum sysfs_registration_status my_sysfs_init(
	int mode)
{
	int status;

	/* Registering a sysfs bus */
	status = bus_register(&my_bus_type);
	if (status)
		return SYSFSREGISTRATON_BUS_FAILED;

	/* Registering a driver */
	status = my_driver_register(&mydriver);
	if (status) {
		bus_unregister(&my_bus_type);
		return SYSFSREGISTRATON_DRIVER_FAILED;
	}

	/* Adding driver attributes (
	 * commands - user views available commands,
	 * input_command - user inputs commands and receives results
	 * )
	 */
	if (mode == 1) {
		status = driver_create_file(&(mydriver.driver),
					    &driver_attr_commands);
		if (status) {
			my_driver_unregister(&mydriver);
			bus_unregister(&my_bus_type);
			return SYSFSREGISTRATON_DRIVER_AC_FAILED;
		}
		status = driver_create_file(&(mydriver.driver),
					    &driver_attr_input_command);
		if (status) {
			my_driver_unregister(&mydriver);
			bus_unregister(&my_bus_type);
			return SYSFSREGISTRATON_DRIVER_AI_FAILED;
		}
	}
	return SYSFSREGISTRATON_OK;
}

void my_sysfs_exit(int mode)
{
	if (mode == 1) {
		driver_remove_file(&(mydriver.driver),
				   &driver_attr_commands);
		driver_remove_file(&(mydriver.driver),
				   &driver_attr_input_command);
	}
	my_driver_unregister(&mydriver);
	bus_unregister(&my_bus_type);
}


