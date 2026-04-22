#include <linux/module.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/mutex.h>

#define DEVICE_NAME "bh1750"
#define CLASS_NAME  "bh1750_class"

/* BH1750 commands */
#define BH1750_POWER_DOWN          0x00
#define BH1750_POWER_ON            0x01
#define BH1750_RESET               0x07
#define BH1750_ONE_TIME_H_RES_MODE 0x20

struct bh1750_dev {
    struct i2c_client *client;
    struct device *dev;

    dev_t devt;
    struct cdev cdev;
    struct device *cdevice;

    struct mutex lock;
};

static struct class *bh1750_class;

static int bh1750_read_lux(struct bh1750_dev *bh, int *lux)
{
    int ret;
    u8 buf[2];
    int raw;

    ret = i2c_smbus_write_byte(bh->client, BH1750_POWER_ON);
    if (ret < 0) 
	{
        dev_err(bh->dev, "BH1750 power on failed\n");
        return ret;
    }

    ret = i2c_smbus_write_byte(bh->client, BH1750_ONE_TIME_H_RES_MODE);
    if (ret < 0) 
	{
        dev_err(bh->dev, "BH1750 start measure failed\n");
        return ret;
    }

    msleep(180);

    ret = i2c_master_recv(bh->client, buf, 2);
    if (ret < 0) 
	{
        dev_err(bh->dev, "BH1750 recv failed\n");
        return ret;
    }
    if (ret != 2) 
	{
        dev_err(bh->dev, "BH1750 recv len invalid: %d\n", ret);
        return -EIO;
    }

    raw = (buf[0] << 8) | buf[1];

    /* lux = raw / 1.2，先用整数近似 */
    *lux = raw * 10 / 12;

    return 0;
}

static int bh1750_open(struct inode *inode, struct file *file)
{
    struct bh1750_dev *bh;

    bh = container_of(inode->i_cdev, struct bh1750_dev, cdev);
    file->private_data = bh;

    return 0;
}

static int bh1750_release(struct inode *inode, struct file *file)
{
    return 0;
}

static ssize_t bh1750_read(struct file *file, char __user *buf,
                           size_t count, loff_t *ppos)
{
    struct bh1750_dev *bh = file->private_data;
    int lux;
    int ret;

    if (count < sizeof(int))
        return -EINVAL;

    mutex_lock(&bh->lock);
    ret = bh1750_read_lux(bh, &lux);
    mutex_unlock(&bh->lock);
    if (ret < 0)
        return ret;

    if (copy_to_user(buf, &lux, sizeof(int)))
        return -EFAULT;

    return sizeof(int);
}

static const struct file_operations bh1750_fops = {
    .owner   = THIS_MODULE,
    .open    = bh1750_open,
    .release = bh1750_release,
    .read    = bh1750_read,
};

static int bh1750_chrdev_register(struct bh1750_dev *bh)
{
    int ret;

    ret = alloc_chrdev_region(&bh->devt, 0, 1, DEVICE_NAME);
    if (ret) 
	{
        dev_err(bh->dev, "alloc_chrdev_region failed\n");
        return ret;
    }

    cdev_init(&bh->cdev, &bh1750_fops);
    bh->cdev.owner = THIS_MODULE;

    ret = cdev_add(&bh->cdev, bh->devt, 1);
    if (ret) 
	{
        dev_err(bh->dev, "cdev_add failed\n");
        unregister_chrdev_region(bh->devt, 1);
        return ret;
    }

    bh->cdevice = device_create(bh1750_class, bh->dev, bh->devt, NULL, DEVICE_NAME);
    if (IS_ERR(bh->cdevice)) 
	{
        ret = PTR_ERR(bh->cdevice);
        dev_err(bh->dev, "device_create failed\n");
        cdev_del(&bh->cdev);
        unregister_chrdev_region(bh->devt, 1);
        return ret;
    }

    return 0;
}

static void bh1750_chrdev_unregister(struct bh1750_dev *bh)
{
    device_destroy(bh1750_class, bh->devt);
    cdev_del(&bh->cdev);
    unregister_chrdev_region(bh->devt, 1);
}

static int bh1750_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
    int ret;
    struct bh1750_dev *bh;

    bh = devm_kzalloc(&client->dev, sizeof(*bh), GFP_KERNEL);
    if (!bh)
        return -ENOMEM;

    bh->client = client;
    bh->dev = &client->dev;
    mutex_init(&bh->lock);

    i2c_set_clientdata(client, bh);

    ret = bh1750_chrdev_register(bh);
    if (ret)
        return ret;

    dev_info(&client->dev, "bh1750 probe success\n");
    return 0;
}

static int bh1750_remove(struct i2c_client *client)
{
    struct bh1750_dev *bh = i2c_get_clientdata(client);

    bh1750_chrdev_unregister(bh);
    dev_info(&client->dev, "bh1750 remove success\n");
    return 0;
}

static const struct of_device_id bh1750_of_match[] = {
    { .compatible = "xlp,light" },
    { }
};
MODULE_DEVICE_TABLE(of, bh1750_of_match);

static const struct i2c_device_id bh1750_id[] = {
    { "bh1750", 0 },
    { }
};
MODULE_DEVICE_TABLE(i2c, bh1750_id);

static struct i2c_driver bh1750_driver = {
    .probe  = bh1750_probe,
    .remove = bh1750_remove,
    .driver = {
        .name = "bh1750",
        .of_match_table = bh1750_of_match,
    },
    .id_table = bh1750_id,
};

static int __init bh1750_init(void)
{
    int ret;

    bh1750_class = class_create(THIS_MODULE, CLASS_NAME);
    if (IS_ERR(bh1750_class))
        return PTR_ERR(bh1750_class);

    ret = i2c_add_driver(&bh1750_driver);
    if (ret) 
	{
        class_destroy(bh1750_class);
        return ret;
    }

    pr_info("bh1750 driver init\n");
    return 0;
}

static void __exit bh1750_exit(void)
{
    i2c_del_driver(&bh1750_driver);
    class_destroy(bh1750_class);
    pr_info("bh1750 driver exit\n");
}

module_init(bh1750_init);
module_exit(bh1750_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("xlp");
MODULE_DESCRIPTION("BH1750 light sensor driver");
