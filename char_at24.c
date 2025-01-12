// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) "%s: " fmt,__func__

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/delay.h>

/* Address pointer is 16 bit. */
#define AT24_FLAG_ADDR16	BIT(7)

#define AT24_24C256_LEN  (262144 / 8)
#define MAX_READ_LEN     1500

struct at24_data {
    u32 byte_len;
    u8 flags;

    struct cdev cdev;
    struct class *class;
    struct device *device;
    dev_t device_number;

    struct mutex lock;

    struct i2c_client *client;
};

struct char_at24_chip_data {
    u32 byte_len;
    u8 flags;
};

static const struct char_at24_chip_data char_at24_data = {  \
    .byte_len = AT24_24C256_LEN, .flags = AT24_FLAG_ADDR16,   \
};

static const struct of_device_id char_at24_of_match[] = {
    { .compatible = "char_at,char_24c256",  .data = &char_at24_data },
    { /* END OF LIST */ }
};
MODULE_DEVICE_TABLE(of, char_at24_of_match);

static unsigned int at24_io_limit = 64;

static ssize_t char_at24_read(struct file *filp, char __user *buff, size_t count, loff_t *f_pos)
{
    struct at24_data *data = filp->private_data;
    struct i2c_client *client = data->client;
    u8 i;
    s32 res;
    ssize_t ret;
    char *src_buf;
    u8 addr_hi = (*f_pos >> 8) & 0xFF;
    u8 addr_lo = *f_pos & 0xFF;

    src_buf = devm_kzalloc(data->device, at24_io_limit, GFP_KERNEL);
    if (!src_buf)
        return -ENOMEM;

    if (mutex_lock_interruptible(&data->lock)) {
        ret = -EINTR;
        goto clean;
    }

    pr_info("Read requested for %zu bytes \n", count);
	pr_info("Current file position = %lld\n", *f_pos);

    /* early exit */
    if (*f_pos >= MAX_READ_LEN) {
        *f_pos = 0;
        ret = 0;
        goto unlock;
    }

    /* Adjust the count*/
    if ((*f_pos + count) > AT24_24C256_LEN) {
        count = AT24_24C256_LEN - *f_pos;
    }
    count = at24_io_limit;

    /* Random read: dummy write to the address that we want to read*/
    res = i2c_smbus_write_byte_data(client, addr_hi, addr_lo);
    if (res < 0) {
        pr_err("error dummy write i2c_smbus_write_byte_data\n");
        ret = -EINTR;// @fix error
        goto unlock;
    }

    for (i = 0; i < count; i++) {
        res = i2c_smbus_read_byte(client);
        if (res < 0) {
            pr_err("error reading\n");
            ret = -EINTR;// @fix error
            goto unlock;
        }
        src_buf[i] = (u8)res;
    }

    src_buf[count - 1] = '\n';

    if (copy_to_user(buff, &src_buf[0], count)) {
        ret = -EFAULT;
        goto unlock;
    }

    *f_pos += count;

    pr_info("Number of bytes succesfully read = %zu", count);
    pr_info("Updated file position = %lld\n", *f_pos);

    mutex_unlock(&data->lock);
    devm_kfree(data->device, src_buf);

    return count;

unlock:
    mutex_unlock(&data->lock);
clean:
    devm_kfree(data->device, src_buf);
    return ret;
}

static ssize_t char_at24_write(struct file *filp, const char __user *buff, size_t count, loff_t *f_pos)
{
    struct at24_data *data = filp->private_data;
    struct i2c_client *client = data->client;
    u8 addr_hi, addr_lo, i;
    u16 address;
    s32 res;
    char *dst_buf;
    ssize_t ret;

    dst_buf = devm_kzalloc(data->device, at24_io_limit, GFP_KERNEL);
    if (!dst_buf)
        return -ENOMEM;

    if (count > at24_io_limit)
        count = at24_io_limit;

    if (*f_pos + count >  AT24_24C256_LEN)
        return -ENOMEM;

    if (mutex_lock_interruptible(&data->lock)) {
        ret = -EINTR;
        goto clean;
    }

    pr_info("Write requested for %zu bytes\n", count);
    pr_info("Current file position = %lld\n", *f_pos);

    /*copy from user*/
    if (copy_from_user(&dst_buf[0], buff, count)) {
        ret = -EFAULT;
        goto unlock;
    }

    address = *f_pos;
    for (i = 0; i < at24_io_limit; i++) {
        addr_hi = address >> 8;
        addr_lo = address;
        res = i2c_smbus_write_word_data(client, addr_hi, (dst_buf[i] << 8 | addr_lo));
        if (res < 0) {
            pr_err("error writing \n");
            ret = -EFAULT;
            goto unlock;
        }
        address++;
        usleep_range(4000, 4500);
    }

    /*update the current file position*/
    *f_pos += count;

    mutex_unlock(&data->lock);

    devm_kfree(data->device, dst_buf);

    return count;

unlock:
    mutex_unlock(&data->lock);
clean:
    devm_kfree(data->device, dst_buf);
    return ret;
}

static int char_at24_open(struct inode *inode, struct file *filp)
{
    struct at24_data *data = container_of(inode->i_cdev, struct at24_data, cdev);

    if (!data)
        return -ENODEV;

    filp->private_data = data;

    pr_info("open was successful\n");
    return 0;
}

static int char_at24_release(struct inode *inode, struct file *filp)
{
    pr_info("release was successful\n");
    return 0;
}

/* file operations of the driver */
static struct file_operations char_at24_fops = {
    .owner = THIS_MODULE,
    .read = char_at24_read,
    .write = char_at24_write,
    .open = char_at24_open,
    .release = char_at24_release,
};

static char *char_at24_devnode(struct device *dev, umode_t *mode)
{
    if (mode != NULL)
        *mode = 0666;

    return NULL;
}

static int char_at24_probe(struct i2c_client *client)
{
    int ret;
    struct at24_data *at24;

    at24 = devm_kzalloc(&client->dev, sizeof(*at24), GFP_KERNEL);
    if (!at24)
        return -ENOMEM;

    ret = alloc_chrdev_region(&at24->device_number, 0, 1, "char_at24");
    if (ret < 0) {
        pr_err("alloc chrdev failed\n");
        goto out;
    }
    pr_info("Device number <major>:<minor> = %d:%d\n", MAJOR(at24->device_number), MINOR(at24->device_number));

    cdev_init(&at24->cdev, &char_at24_fops);
    at24->cdev.owner = THIS_MODULE;

    ret = cdev_add(&at24->cdev, at24->device_number, 1);
    if (ret < 0) {
        pr_err("cdev add failed\n");
        goto unreg_chrdev;
    }

    at24->class = class_create(THIS_MODULE, "char_at24_class");
    if (IS_ERR(at24->class)) {
        pr_err("class creation failed\n");
        ret = PTR_ERR(at24->class);
        goto cdev_del;
    }

    at24->class->devnode = char_at24_devnode;

    at24->device = device_create(at24->class, NULL, at24->device_number, NULL, "char_at24");
    if (IS_ERR(at24->device)) {
        pr_err("device created failed\n");
        ret = PTR_ERR(at24->device);
        goto class_del;
    }

    at24->client = client;

    i2c_set_clientdata(client, at24);

    pr_info("probe was successful\n");
    return 0;

class_del:
    class_destroy(at24->class);
cdev_del:
    cdev_del(&at24->cdev);
unreg_chrdev:
    unregister_chrdev_region(at24->device_number, 1);
out:
    pr_info("Module insertion failed \n"); 

    return ret;
}

static void char_at24_remove(struct i2c_client *client)
{
    struct at24_data *at24 = i2c_get_clientdata(client);
    device_destroy(at24->class, at24->device_number);
    class_destroy(at24->class);
    cdev_del(&at24->cdev);
    unregister_chrdev_region(at24->device_number, 1);
    pr_info("remove was successful\n");
}

static struct i2c_driver char_at24_driver = {
    .driver = {
        .name = "char_at24",
        .of_match_table = char_at24_of_match,
    },
    .probe_new = char_at24_probe,
    .remove = char_at24_remove,
};

module_i2c_driver(char_at24_driver);

MODULE_AUTHOR("Reyders Morales");
MODULE_DESCRIPTION("Simple Linux character device driver for at24 i2c memory");
MODULE_LICENSE("GPL");
