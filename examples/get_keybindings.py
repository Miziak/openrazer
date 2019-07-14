from openrazer.client import DeviceManager

# Create a DeviceManager. This is used to get specific devices
device_manager = DeviceManager()


print("Found {} Razer devices\n".format(len(device_manager.devices)))


# Iterate over each device and pretty out some standard information about each
for device in device_manager.devices:
    if device.type == 'keypad':
        print("{}:".format(device.name))
        print("   type: {}".format(device.type))
        print("   serial: {}".format(device.serial))
        print("   firmware version: {}".format(device.firmware_version))
        print("   driver version: {}".format(device.driver_version))
        print(device.translations)
        print()
