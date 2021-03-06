/*
 * Copyright (c) 2015 Tim Theede <pez2001@voyagerproject.de>
 *               2015 Terry Cain <terry@terrys-home.co.uk>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/hid.h>


#include "razercommon.h"

/**
 * Send USB control report to the keyboard
 * USUALLY index = 0x02
 * FIREFLY is 0
 */
int razer_send_control_msg(struct usb_device *usb_dev,void const *data, uint report_index, ulong wait_min, ulong wait_max)
{
    uint request = HID_REQ_SET_REPORT; // 0x09
    uint request_type = USB_TYPE_CLASS | USB_RECIP_INTERFACE | USB_DIR_OUT; // 0x21
    uint value = 0x300;
    uint size = RAZER_USB_REPORT_LEN;
    char *buf;
    int len;

    buf = kmemdup(data, size, GFP_KERNEL);
    if (buf == NULL)
        return -ENOMEM;

    // Send usb control message
    len = usb_control_msg(usb_dev, usb_sndctrlpipe(usb_dev, 0),
                          request,      // Request
                          request_type, // RequestType
                          value,        // Value
                          report_index, // Index
                          buf,          // Data
                          size,         // Length
                          USB_CTRL_SET_TIMEOUT);

    // Wait
    usleep_range(wait_min, wait_max);

    kfree(buf);
    if(len!=size)
        printk(KERN_WARNING "razer driver: Device data transfer failed.");

    return ((len < 0) ? len : ((len != size) ? -EIO : 0));
}

/**
 * Get a response from the razer device
 *
 * Makes a request like normal, this must change a variable in the device as then we
 * tell it give us data and it gives us a report.
 *
 * Supported Devices:
 *   Razer Chroma
 *   Razer Mamba
 *   Razer BlackWidow Ultimate 2013*
 *   Razer Firefly*
 *
 * Request report is the report sent to the device specifying what response we want
 * Response report will get populated with a response
 *
 * Returns 0 when successful, 1 if the report length is invalid.
 */
int razer_get_usb_response(struct usb_device *usb_dev, uint report_index, struct razer_report* request_report, uint response_index, struct razer_report* response_report, ulong wait_min, ulong wait_max)
{
    uint request = HID_REQ_GET_REPORT; // 0x01
    uint request_type = USB_TYPE_CLASS | USB_RECIP_INTERFACE | USB_DIR_IN; // 0xA1
    uint value = 0x300;

    uint size = RAZER_USB_REPORT_LEN; // 0x90
    int len;
    int retval;
    int result = 0;
    char *buf;

    buf = kzalloc(sizeof(struct razer_report), GFP_KERNEL);
    if (buf == NULL)
        return -ENOMEM;

    // Send the request to the device.
    // TODO look to see if index needs to be different for the request and the response
    retval = razer_send_control_msg(usb_dev, request_report, report_index, wait_min, wait_max);

    // Now ask for response
    len = usb_control_msg(usb_dev, usb_rcvctrlpipe(usb_dev, 0),
                          request,         // Request
                          request_type,    // RequestType
                          value,           // Value
                          response_index,  // Index
                          buf,             // Data
                          size,
                          USB_CTRL_SET_TIMEOUT);

    memcpy(response_report, buf, sizeof(struct razer_report));
    kfree(buf);

    // Error if report is wrong length
    if(len != 90) {
        printk(KERN_WARNING "razer driver: Invalid USB response. USB Report length: %d\n", len);
        result = 1;
    }

    return result;
}

/**
 * Calculate the checksum for the usb message
 *
 * Checksum byte is stored in the 2nd last byte in the messages payload.
 * The checksum is generated by XORing all the bytes in the report starting
 * at byte number 2 (0 based) and ending at byte 88.
 */
unsigned char razer_calculate_crc(struct razer_report *report)
{
    /*second to last byte of report is a simple checksum*/
    /*just xor all bytes up with overflow and you are done*/
    unsigned char crc = 0;
    unsigned char *_report = (unsigned char*)report;

    unsigned int i;
    for(i = 2; i < 88; i++) {
        crc ^= _report[i];
    }

    return crc;
}

/**
 * Get initialised razer report
 */
struct razer_report get_razer_report(unsigned char command_class, unsigned char command_id, unsigned char data_size)
{
    struct razer_report new_report = {0};
    memset(&new_report, 0, sizeof(struct razer_report));

    new_report.status = 0x00;
    new_report.transaction_id.id = 0xFF;
    new_report.remaining_packets = 0x00;
    new_report.protocol_type = 0x00;
    new_report.command_class = command_class;
    new_report.command_id.id = command_id;
    new_report.data_size = data_size;

    return new_report;
}

/**
 * Get empty razer report
 */
struct razer_report get_empty_razer_report(void)
{
    struct razer_report new_report = {0};
    memset(&new_report, 0, sizeof(struct razer_report));

    return new_report;
}

/**
 * Find key translations for given device
 * @translations: list_head of all devices translations
 * @id: id of device
 *
 * @return: struct with device translations
 */
static struct razer_device_translations *razer_get_device(struct razer_device_translations *translations, u16 id)
{
    struct razer_device_translations *ptr;
    list_for_each_entry(ptr, &translations->list, list) {
        if (ptr->id == id) {
            return ptr;
        }
    }

    return NULL;
}

/**
 * free mamory of array containing translations for device
 * @translations: pointer to device translations
 */
static void razer_free_translations(struct razer_device_translations *translations)
{
    size_t i;
    for(i = 0; i < translations->length; i++) {
        kfree(translations->translations[i]);
    }
    kfree(translations->translations);
}

/**
 * allocate mamory for array containing translations device
 * @translations: pointer to device translations
 * @length: total count of translations for device
 */
static void razer_alloc_translations(struct razer_device_translations *translations, size_t length)
{
    size_t i;
    translations->length = length;
    translations->translations = kzalloc(sizeof(struct razer_key_translation*) * length, GFP_KERNEL);

    for (i = 0; i < length; i++) {
        translations->translations[i] = kzalloc(sizeof(struct razer_key_translation), GFP_KERNEL);
    }
}

/**
 * count translations for device only for logging and debugging
 * @translations: pointer to device translations
 * @id: id of device
 */
static u8 razer_count_translations(struct razer_device_translations *translations, u16 id)
{
    u8 i = 0;
    struct razer_device_translations *ptr;
    list_for_each_entry(ptr, &translations->list, list) {
        if (ptr->id == id) {
            i++;
        }
    }

    return i;
}

/**
 * modify translations, this function have some different
 * "modes" of work depending on input:
 *   1. device don't have translations -> allocate memory for it and set translations
 *   2. device have translations and their count is diffrent than provided -> free mam then allocate new one then set translations
 *   3. device have translations and their count is equal provided -> just set new translations
 *   4. buffer contain one byte(it's value don't matter) -> delete bindings for device(default bindings)
 * "protocol" of setting translations:
 *   to set translations you need write array of u16 to *button_translations* file contained in
 *   "/sys/bus/hid/drivers/<razer(mouse|kbd)/<DEVICE ID>/button_translations" each binding is
 *   pair of u16 integeras representing first: keycode to remap, second: button destination keycode
 *   eg. "echo -n -e \\x02\\x00\\x1E\\x00\\x03\\x00\\x30\\x00 > button_translations" translate my
 *   razer naga keypad buttons:
 *     1 -> a
 *     2 -> b
 *     rest -> default
 *  or write one byte to delete bindings
 *  eg. echo -n -e \\x00 > button_translations
 *
 * @translations: list_head of all devices translations
 * @id: id of device
 * @buff: buffer of input data
 * @count: size of data in buffer
 * @return:
 *   0 -> translations successfully changed
 *   1 -> deleted translations for device
 *   2 -> error binding buffer is not pairs of u16
 */
u8 razer_set_translations(struct razer_device_translations *translations, u16 id, const char *buf, size_t count)
{
    size_t offset;
    size_t bindingSize = sizeof(u16) * 2;
    size_t bindingsCount = count / bindingSize;
    struct razer_device_translations *it;

    // here we delete bindings for device
    if (count == sizeof(u8)) {
        if ((it = razer_get_device(translations, id)) != NULL) {
            razer_free_translations(it);
            list_del(&it->list);
            kfree(it);

            printk("razercommon: [Translations] clear translations for device %d count %d should be 0!\n", (int)id, (int)razer_count_translations(translations, id));
            return 1;
        }
    }

    // here we check buffer is pairs of u16
    if (count % sizeof(u16) != 0) {
        return 2;
    }

    if ((it = razer_get_device(translations, id)) == NULL) {
        // here we allocate bindings if device dont have any
        it = kzalloc(sizeof(struct razer_device_translations), GFP_KERNEL);
        it->id = id;
        razer_alloc_translations(it, bindingsCount);
        INIT_LIST_HEAD(&it->list);
        list_add(&it->list, &translations->list);
    }
    if ((it = razer_get_device(translations, id)) != NULL && it->length != bindingsCount) {
        // here we change baindings when count of existing bindings is different than new
        razer_free_translations(it);
        razer_alloc_translations(it, bindingsCount);
    }

    // bindings set
    for (offset = 0; offset < bindingsCount; offset++) {
        memcpy(it->translations[offset], buf + (offset * bindingSize), bindingSize);
        it->translations[offset]->flags = 0;
    }

    printk("razercommon: [Translations] %d is count of translations for device id %d\n", (int)razer_count_translations(translations, id), (int)id);

    return 0;
}

/**
 * dump all bindings for device as bytearray for read by end user
 * @translations: list_head of all devices translations
 * @id: id of device
 * @buff: buffer to write output
 * @return: length of flushed data
 */
size_t razer_get_translations(struct razer_device_translations *translations, u16 id, char *buf)
{
    struct razer_device_translations *device;
    size_t bindingSize = sizeof(u16) * 2;

    if ((device = razer_get_device(translations, id)) != NULL) {
        size_t offset, size = 0;
        for (offset = 0; offset < device->length; offset++) {
            memcpy(buf + (offset * bindingSize), device->translations[offset], bindingSize);
            size += bindingSize;
        }

        return size;
    }

    strcpy(buf, "\0");
    return 1;
}

/**
 * get translation for device key
 * @translations: list_head of all devices translations
 * @id: id of device
 * @key: keycode of pressed button
 * @return: translated keycode for pressed button
 */
struct razer_key_translation *razer_get_translation(struct razer_device_translations *translations, u16 id, u16 key)
{
    struct razer_device_translations *device = razer_get_device(translations, id);

    if (device != NULL) {
        size_t i;
        for(i = 0; i < device->length; i++) {
            if (device->translations[i]->from == key) {
                return device->translations[i];
            }
        }
    }

    return NULL;
}


/**
 * init devices list
 */
void razer_init_translations(struct razer_device_translations *translations)
{
    INIT_LIST_HEAD(&translations->list);
}

/**
 * cleanup function empty list of devices and free memory of all stored translations
 * @translations: list_head of all devices translations
 */
void razer_cleanup_translations(struct razer_device_translations *translations)
{
    struct razer_device_translations *ptr, *next;

    if (list_empty(&translations->list) != 0) {
        return;
    }

    list_for_each_entry_safe(ptr, next, &translations->list, list) {
        razer_free_translations(ptr);
        list_del(&ptr->list);
        kfree(ptr);
    }
}

/**
 * Print report to syslog
 */
void print_erroneous_report(struct razer_report* report, char* driver_name, char* message)
{
    printk(KERN_WARNING "%s: %s. Start Marker: %02x id: %02x Num Params: %02x Reserved: %02x Command: %02x Params: %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x .\n",
           driver_name,
           message,
           report->status,
           report->transaction_id.id,
           report->data_size,
           report->command_class,
           report->command_id.id,
           report->arguments[0], report->arguments[1], report->arguments[2], report->arguments[3], report->arguments[4], report->arguments[5],
           report->arguments[6], report->arguments[7], report->arguments[8], report->arguments[9], report->arguments[10], report->arguments[11],
           report->arguments[12], report->arguments[13], report->arguments[14], report->arguments[15]);
}

/**
 * Clamp a value to a min,max
 */
unsigned char clamp_u8(unsigned char value, unsigned char min, unsigned char max)
{
    if(value > max)
        return max;
    if(value < min)
        return min;
    return value;
}
unsigned short clamp_u16(unsigned short value, unsigned short min, unsigned short max)
{
    if(value > max)
        return max;
    if(value < min)
        return min;
    return value;
}


int razer_send_control_msg_old_device(struct usb_device *usb_dev,void const *data, uint report_value, uint report_index, uint report_size, ulong wait_min, ulong wait_max)
{
    uint request = HID_REQ_SET_REPORT; // 0x09
    uint request_type = USB_TYPE_CLASS | USB_RECIP_INTERFACE | USB_DIR_OUT; // 0x21
    char *buf;
    int len;

    buf = kmemdup(data, report_size, GFP_KERNEL);
    if (buf == NULL)
        return -ENOMEM;

    // Send usb control message
    len = usb_control_msg(usb_dev, usb_sndctrlpipe(usb_dev, 0),
                          request,      // Request
                          request_type, // RequestType
                          report_value, // Value
                          report_index, // Index
                          buf,          // Data
                          report_size,  // Length
                          USB_CTRL_SET_TIMEOUT);

    // Wait
    usleep_range(wait_min, wait_max);

    kfree(buf);
    if(len!=report_size)
        printk(KERN_WARNING "razer driver: Device data transfer failed.");

    return ((len < 0) ? len : ((len != report_size) ? -EIO : 0));
}
















