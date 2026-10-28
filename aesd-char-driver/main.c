/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 *
 * Based on the implementation of the "scull" device driver, found in
 * Linux Device Drivers example code.
 *
 * @author Dan Walkes
 * @date 2019-10-22
 * @copyright Copyright (c) 2019
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h> // file_operations
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include "aesd_ioctl.h"
#include "aesdchar.h"
int aesd_major =   0; // use dynamic major
int aesd_minor =   0;

MODULE_AUTHOR("julian-werder");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("AESD circular buffer character device driver");

struct aesd_dev aesd_device;

static int aesd_open(struct inode *inode, struct file *filp)
{
    PDEBUG("open");
    filp->private_data = container_of(inode->i_cdev, struct aesd_dev, cdev);
    return 0;
}

static int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    /* Nothing to release, the device data lives until the module is unloaded */
    return 0;
}

static ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                loff_t *f_pos)
{
    struct aesd_dev *dev = filp->private_data;
    struct aesd_buffer_entry *entry;
    size_t entry_offset = 0;
    size_t nbytes;
    ssize_t retval = 0;
    PDEBUG("read %zu bytes with offset %lld",count,*f_pos);

    if (mutex_lock_interruptible(&dev->lock)) {
        return -ERESTARTSYS;
    }

    entry = aesd_circular_buffer_find_entry_offset_for_fpos(&dev->buffer, (size_t)*f_pos, &entry_offset);
    if (entry == NULL) {
        /* Nothing at this position, end of file */
        mutex_unlock(&dev->lock);
        return 0;
    }

    nbytes = min(count, entry->size - entry_offset);
    if (copy_to_user(buf, entry->buffptr + entry_offset, nbytes)) {
        mutex_unlock(&dev->lock);
        return -EFAULT;
    }

    *f_pos += nbytes;
    retval = (ssize_t)nbytes;
    mutex_unlock(&dev->lock);
    return retval;
}

static ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                loff_t *f_pos)
{
    struct aesd_dev *dev = filp->private_data;
    struct aesd_buffer_entry *oldest;
    char *new_buf;
    PDEBUG("write %zu bytes with offset %lld",count,*f_pos);

    if (count == 0) {
        return 0;
    }

    if (mutex_lock_interruptible(&dev->lock)) {
        return -ERESTARTSYS;
    }

    /* Grow through a temporary so the old partial write survives a failed krealloc */
    new_buf = krealloc(dev->partial.buffptr, dev->partial.size + count, GFP_KERNEL);
    if (new_buf == NULL) {
        mutex_unlock(&dev->lock);
        return -ENOMEM;
    }
    dev->partial.buffptr = new_buf;

    if (copy_from_user(new_buf + dev->partial.size, buf, count)) {
        /* partial.size is unchanged, so the failed bytes are simply not part of the write */
        mutex_unlock(&dev->lock);
        return -EFAULT;
    }

    if (memchr(new_buf + dev->partial.size, '\n', count) != NULL) {
        dev->partial.size += count;
        if (dev->buffer.full) {
            /* The add below overwrites the oldest entry, so free its memory first */
            oldest = &dev->buffer.entry[dev->buffer.in_offs];
            kfree(oldest->buffptr);
        }
        aesd_circular_buffer_add_entry(&dev->buffer, &dev->partial);
        dev->partial.buffptr = NULL;
        dev->partial.size = 0;
    } else {
        dev->partial.size += count;
    }

    mutex_unlock(&dev->lock);
    return (ssize_t)count;
}

static loff_t aesd_llseek(struct file *filp, loff_t off, int whence)
{
    struct aesd_dev *dev = filp->private_data;
    struct aesd_buffer_entry *entry;
    uint8_t index;
    loff_t total = 0;
    PDEBUG("llseek offset %lld whence %d", off, whence);

    if (mutex_lock_interruptible(&dev->lock)) {
        return -ERESTARTSYS;
    }

    /* The partial write does not count, it is not readable yet */
    AESD_CIRCULAR_BUFFER_FOREACH(entry, &dev->buffer, index) {
        total += entry->size;
    }
    mutex_unlock(&dev->lock);

    return fixed_size_llseek(filp, off, whence, total);
}

static long aesd_unlocked_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct aesd_dev *dev = filp->private_data;
    struct aesd_seekto seekto;
    unsigned int count;
    unsigned int i;
    loff_t new_pos = 0;
    PDEBUG("ioctl cmd %u", cmd);

    if (_IOC_TYPE(cmd) != AESD_IOC_MAGIC || _IOC_NR(cmd) > AESDCHAR_IOC_MAXNR || cmd != AESDCHAR_IOCSEEKTO) {
        return -ENOTTY;
    }

    if (copy_from_user(&seekto, (const void __user *)arg, sizeof(seekto))) {
        return -EFAULT;
    }

    if (mutex_lock_interruptible(&dev->lock)) {
        return -ERESTARTSYS;
    }

    if (dev->buffer.full) {
        count = AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    } else {
        count = (dev->buffer.in_offs - dev->buffer.out_offs + AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED) %
                AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    }

    if (seekto.write_cmd >= count ||
        seekto.write_cmd_offset >= dev->buffer.entry[(dev->buffer.out_offs + seekto.write_cmd) %
                                                     AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED].size) {
        mutex_unlock(&dev->lock);
        return -EINVAL;
    }

    for (i = 0; i < seekto.write_cmd; i++) {
        new_pos += dev->buffer.entry[(dev->buffer.out_offs + i) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED].size;
    }
    new_pos += seekto.write_cmd_offset;

    filp->f_pos = new_pos;
    mutex_unlock(&dev->lock);
    return 0;
}

struct file_operations aesd_fops = {
    .owner =          THIS_MODULE,
    .llseek =         aesd_llseek,
    .read =           aesd_read,
    .write =          aesd_write,
    .unlocked_ioctl = aesd_unlocked_ioctl,
    .open =           aesd_open,
    .release =        aesd_release,
};

static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err, devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &aesd_fops;
    err = cdev_add (&dev->cdev, devno, 1);
    if (err) {
        printk(KERN_ERR "Error %d adding aesd cdev", err);
    }
    return err;
}



static int aesd_init_module(void)
{
    dev_t dev = 0;
    int result;
    result = alloc_chrdev_region(&dev, aesd_minor, 1,
            "aesdchar");
    aesd_major = MAJOR(dev);
    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }
    memset(&aesd_device,0,sizeof(struct aesd_dev));

    aesd_circular_buffer_init(&aesd_device.buffer);
    mutex_init(&aesd_device.lock);

    result = aesd_setup_cdev(&aesd_device);

    if( result ) {
        unregister_chrdev_region(dev, 1);
    }
    return result;

}

static void aesd_cleanup_module(void)
{
    dev_t devno = MKDEV(aesd_major, aesd_minor);
    struct aesd_buffer_entry *entry;
    uint8_t index;

    cdev_del(&aesd_device.cdev);

    AESD_CIRCULAR_BUFFER_FOREACH(entry, &aesd_device.buffer, index) {
        kfree(entry->buffptr);
    }
    kfree(aesd_device.partial.buffptr);

    unregister_chrdev_region(devno, 1);
}



module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
