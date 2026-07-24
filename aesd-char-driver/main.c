/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h> // file_operations
#include <linux/slab.h> // kmalloc, kfree
#include <linux/uaccess.h> // copy_to_user, copy_from_user
#include "aesdchar.h"
#include "aesd_ioctl.h"

int aesd_major =   0; // use dynamic major
int aesd_minor =   0;

MODULE_AUTHOR("Your Name Here"); /** TODO: fill in your name **/
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

int aesd_open(struct inode *inode, struct file *filp)
{
    struct aesd_dev *dev;
    PDEBUG("open");
    
    // Get the pointer to our custom device structure
    dev = container_of(inode->i_cdev, struct aesd_dev, cdev);
    filp->private_data = dev; 
    
    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    return 0;
}

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                loff_t *f_pos)
{
    struct aesd_dev *dev = filp->private_data;
    struct aesd_buffer_entry *entry;
    size_t entry_offset_byte_rtn;
    size_t bytes_to_read;
    ssize_t retval = 0;

    PDEBUG("read %zu bytes with offset %lld", count, *f_pos);

    if (mutex_lock_interruptible(&dev->lock)) {
        return -ERESTARTSYS;
    }

    // Find the specific circular buffer entry based on the global file offset
    entry = aesd_circular_buffer_find_entry_offset_for_fpos(&dev->buffer, *f_pos, &entry_offset_byte_rtn);
    
    if (entry == NULL) {
        retval = 0; // EOF
        goto out;
    }

    // Implement Partial Read Rule: Only return data from the current entry
    bytes_to_read = entry->size - entry_offset_byte_rtn;
    if (bytes_to_read > count) {
        bytes_to_read = count;
    }

    if (copy_to_user(buf, entry->buffptr + entry_offset_byte_rtn, bytes_to_read)) {
        retval = -EFAULT;
        goto out;
    }

    *f_pos += bytes_to_read;
    retval = bytes_to_read;

out:
    mutex_unlock(&dev->lock);
    return retval;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                loff_t *f_pos)
{
    struct aesd_dev *dev = filp->private_data;
    ssize_t retval = -ENOMEM;
    char *write_data;

    PDEBUG("write %zu bytes with offset %lld", count, *f_pos);

    write_data = kmalloc(count, GFP_KERNEL);
    if (!write_data) return -ENOMEM;

    if (copy_from_user(write_data, buf, count)) {
        kfree(write_data);
        return -EFAULT;
    }

    if (mutex_lock_interruptible(&dev->lock)) {
        kfree(write_data);
        return -ERESTARTSYS;
    }

    // Append new data to our partial entry buffer
    if (dev->partial_entry.size == 0) {
        dev->partial_entry.buffptr = kmalloc(count, GFP_KERNEL);
        if (!dev->partial_entry.buffptr) {
            retval = -ENOMEM;
            goto out;
        }
        memcpy((char *)dev->partial_entry.buffptr, write_data, count);
        dev->partial_entry.size = count;
    } else {
        dev->partial_entry.buffptr = krealloc(dev->partial_entry.buffptr, dev->partial_entry.size + count, GFP_KERNEL);
        if (!dev->partial_entry.buffptr) {
            retval = -ENOMEM;
            goto out;
        }
        memcpy((char *)dev->partial_entry.buffptr + dev->partial_entry.size, write_data, count);
        dev->partial_entry.size += count;
    }

    // Check if the command is terminated by '\n'
    if (memchr(write_data, '\n', count)) {
        // If the circular buffer is full, free the oldest entry to avoid memory leak
        if (dev->buffer.full) {
            kfree(dev->buffer.entry[dev->buffer.in_offs].buffptr);
        }
        
        aesd_circular_buffer_add_entry(&dev->buffer, &dev->partial_entry);
        
        // Reset the partial entry for the next write
        dev->partial_entry.buffptr = NULL;
        dev->partial_entry.size = 0;
    }

    retval = count;

out:
    mutex_unlock(&dev->lock);
    kfree(write_data);
    return retval;
}

loff_t aesd_llseek(struct file *filp, loff_t off, int whence)
{
    struct aesd_dev *dev = filp->private_data;
    loff_t retval;
    size_t total_size = 0;
    struct aesd_buffer_entry *entry;
    uint8_t index;

    if (mutex_lock_interruptible(&dev->lock)) {
        return -ERESTARTSYS;
    }

    // Iterate through circular buffer to calculate total device size
    AESD_CIRCULAR_BUFFER_FOREACH(entry, &dev->buffer, index) {
        if (entry->buffptr) {
            total_size += entry->size;
        }
    }

    // Use kernel helper for fixed size seek logic
    retval = fixed_size_llseek(filp, off, whence, total_size);

    mutex_unlock(&dev->lock);
    return retval;
}

long aesd_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct aesd_dev *dev = filp->private_data;
    long retval = 0;
    struct aesd_seekto seekto;
    size_t current_offset = 0;
    uint8_t count = 0;
    uint8_t num_entries = 0;
    uint8_t i;
    struct aesd_buffer_entry *entry;

    if (_IOC_TYPE(cmd) != AESD_IOC_MAGIC) return -ENOTTY;
    if (_IOC_NR(cmd) > AESDCHAR_IOC_MAXNR) return -ENOTTY;

    switch(cmd) {
        case AESDCHAR_IOCSEEKTO:
            if (copy_from_user(&seekto, (const void __user *)arg, sizeof(seekto))) {
                return -EFAULT;
            }

            if (mutex_lock_interruptible(&dev->lock)) {
                return -ERESTARTSYS;
            }

            // Count how many entries are actually in the buffer
            AESD_CIRCULAR_BUFFER_FOREACH(entry, &dev->buffer, i) {
                num_entries++;
            }

            // Check if requested command index is out of bounds
            if (seekto.write_cmd >= num_entries) {
                retval = -EINVAL;
            } else {
                // Find the specific command and calculate the byte offset
                AESD_CIRCULAR_BUFFER_FOREACH(entry, &dev->buffer, i) {
                    if (count == seekto.write_cmd) {
                        // Check if requested offset is out of bounds for THIS command
                        if (seekto.write_cmd_offset >= entry->size) {
                            retval = -EINVAL;
                        } else {
                            filp->f_pos = current_offset + seekto.write_cmd_offset;
                        }
                        break;
                    }
                    current_offset += entry->size;
                    count++;
                }
            }

            mutex_unlock(&dev->lock);
            break;

        default:
            retval = -ENOTTY;
            break;
    }

    return retval;
}

struct file_operations aesd_fops = {
    .owner =    THIS_MODULE,
    .read =     aesd_read,
    .write =    aesd_write,
    .open =     aesd_open,
    .release =  aesd_release,
    .llseek =   aesd_llseek,
    .unlocked_ioctl = aesd_ioctl,
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

int aesd_init_module(void)
{
    dev_t dev = 0;
    int result;
    result = alloc_chrdev_region(&dev, aesd_minor, 1, "aesdchar");
    aesd_major = MAJOR(dev);
    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }
    memset(&aesd_device,0,sizeof(struct aesd_dev));

    aesd_circular_buffer_init(&aesd_device.buffer);
    mutex_init(&aesd_device.lock);
    aesd_device.partial_entry.buffptr = NULL;
    aesd_device.partial_entry.size = 0;

    result = aesd_setup_cdev(&aesd_device);

    if( result ) {
        unregister_chrdev_region(dev, 1);
    }
    return result;
}

void aesd_cleanup_module(void)
{
    dev_t devno = MKDEV(aesd_major, aesd_minor);
    struct aesd_buffer_entry *entry;
    uint8_t index;

    cdev_del(&aesd_device.cdev);

    AESD_CIRCULAR_BUFFER_FOREACH(entry, &aesd_device.buffer, index) {
        if (entry->buffptr) {
            kfree(entry->buffptr);
        }
    }

    if (aesd_device.partial_entry.buffptr) {
        kfree(aesd_device.partial_entry.buffptr);
    }

    mutex_destroy(&aesd_device.lock);
    unregister_chrdev_region(devno, 1);
}

module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
