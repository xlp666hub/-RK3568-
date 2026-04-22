#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/i2c.h>
#include <linux/types.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/mutex.h>

#define DEVICE_NAME "mpu6050"
#define CLASS_NAME  "mpu6050_class"


#define MPU6050_REG_WHO_AM_I     0x75
#define MPU6050_REG_PWR_MGMT_1   0x6B
#define MPU6050_REG_ACCEL_XOUT_H 0x3B

#define MPU6050_WHO_AM_I_VALUE   0x70

struct mpu6050_frame {
    s16 accel_x;
    s16 accel_y;
    s16 accel_z;
    s16 temp_raw;
    s16 gyro_x;
    s16 gyro_y;
    s16 gyro_z;
};

struct mpu6050_dev {
    dev_t devid;
    struct cdev cdev;
    struct device *device;
    struct i2c_client *client;
    struct mutex lock;
};

static struct class *mpu6050_class;

static int mpu6050_read_frame(struct mpu6050_dev *dev, struct mpu6050_frame *frame)
{
    int ret;
    u8 raw[14];

    ret = i2c_smbus_read_i2c_block_data(dev->client,
                                        MPU6050_REG_ACCEL_XOUT_H,
                                        14,
                                        raw);
    if (ret < 0) 
	{
        dev_err(&dev->client->dev, "failed to read sensor data\n");
        return ret;
    }

    if (ret != 14) 
	{
        dev_err(&dev->client->dev, "invalid read len: %d\n", ret);
        return -EIO;
    }

    frame->accel_x = (s16)((raw[0]  << 8) | raw[1]);
    frame->accel_y = (s16)((raw[2]  << 8) | raw[3]);
    frame->accel_z = (s16)((raw[4]  << 8) | raw[5]);
    frame->temp_raw = (s16)((raw[6] << 8) | raw[7]);
    frame->gyro_x  = (s16)((raw[8]  << 8) | raw[9]);
    frame->gyro_y  = (s16)((raw[10] << 8) | raw[11]);
    frame->gyro_z  = (s16)((raw[12] << 8) | raw[13]);

    return 0;
}

static int mpu6050_open(struct inode *inode, struct file *filp)
{
    struct mpu6050_dev *dev;

    dev = container_of(inode->i_cdev, struct mpu6050_dev, cdev);
    filp->private_data = dev;

    return 0;
}

static ssize_t mpu6050_read(struct file *filp, char __user *buf,
                            size_t count, loff_t *ppos)
{
    int ret;
    struct mpu6050_frame frame;
    struct mpu6050_dev *dev = filp->private_data;

    if (count < sizeof(frame))
        return -EINVAL;

    mutex_lock(&dev->lock);
    ret = mpu6050_read_frame(dev, &frame);
    mutex_unlock(&dev->lock);
    if (ret < 0)
        return ret;

    if (copy_to_user(buf, &frame, sizeof(frame)))
        return -EFAULT;

    return sizeof(frame);
}

static int mpu6050_release(struct inode *inode, struct file *filp)
{
    return 0;
}

static const struct file_operations mpu6050_fops = {
    .owner   = THIS_MODULE,
    .open    = mpu6050_open,
    .read    = mpu6050_read,
    .release = mpu6050_release,
};

static int mpu6050_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
    int ret;
    int whoami;
    struct mpu6050_dev *dev;

    dev_info(&client->dev, "mpu6050 probe enter\n");

    dev = devm_kzalloc(&client->dev, sizeof(*dev), GFP_KERNEL);
    if (!dev)
        return -ENOMEM;

    dev->client = client;
    mutex_init(&dev->lock);

    i2c_set_clientdata(client, dev);

    whoami = i2c_smbus_read_byte_data(client, MPU6050_REG_WHO_AM_I);
    if (whoami < 0) 
	{
        dev_err(&client->dev, "failed to read WHO_AM_I\n");
        return whoami;
    }

    dev_info(&client->dev, "WHO_AM_I = 0x%x\n", whoami);

    if (whoami != MPU6050_WHO_AM_I_VALUE) 
	{
        dev_err(&client->dev, "unexpected WHO_AM_I value: 0x%x\n", whoami);
        return -ENODEV;
    }

    ret = i2c_smbus_write_byte_data(client, MPU6050_REG_PWR_MGMT_1, 0x00);
    if (ret < 0) 
	{
        dev_err(&client->dev, "failed to wake up mpu6050\n");
        return ret;
    }

    ret = alloc_chrdev_region(&dev->devid, 0, 1, DEVICE_NAME);
    if (ret < 0) 
	{
        dev_err(&client->dev, "alloc_chrdev_region failed\n");
        return ret;
    }

    cdev_init(&dev->cdev, &mpu6050_fops);
    dev->cdev.owner = THIS_MODULE;

    ret = cdev_add(&dev->cdev, dev->devid, 1);
    if (ret < 0) 
	{
        dev_err(&client->dev, "cdev_add failed\n");
        goto err_unregister_region;
    }

    dev->device = device_create(mpu6050_class, NULL, dev->devid, NULL, DEVICE_NAME);
    if (IS_ERR(dev->device)) 
	{
        ret = PTR_ERR(dev->device);
        dev_err(&client->dev, "device_create failed\n");
        goto err_del_cdev;
    }

    dev_info(&client->dev, "mpu6050 probe success\n");
    return 0;

err_del_cdev:
    cdev_del(&dev->cdev);
err_unregister_region:
    unregister_chrdev_region(dev->devid, 1);
    return ret;
}

static int mpu6050_remove(struct i2c_client *client)
{
    struct mpu6050_dev *dev = i2c_get_clientdata(client);

    device_destroy(mpu6050_class, dev->devid);
    cdev_del(&dev->cdev);
    unregister_chrdev_region(dev->devid, 1);

    dev_info(&client->dev, "mpu6050 removed\n");
    return 0;
}

static const struct of_device_id mpu6050_of_match[] = {
    { .compatible = "xlp,mpu6050" },
    { }
};
MODULE_DEVICE_TABLE(of, mpu6050_of_match);

static const struct i2c_device_id mpu6050_id[] = {
    { "mpu6050", 0 },
    { }
};
MODULE_DEVICE_TABLE(i2c, mpu6050_id);

static struct i2c_driver mpu6050_driver = {
    .probe    = mpu6050_probe,
    .remove   = mpu6050_remove,
    .id_table = mpu6050_id,
    .driver   = {
        .name = "mpu6050_driver",
        .of_match_table = mpu6050_of_match,
    },
};

static int __init mpu6050_init(void)
{
    int ret;

    mpu6050_class = class_create(THIS_MODULE, CLASS_NAME);
    if (IS_ERR(mpu6050_class))
        return PTR_ERR(mpu6050_class);

    ret = i2c_add_driver(&mpu6050_driver);
    if (ret) 
	{
        class_destroy(mpu6050_class);
        return ret;
    }

    pr_info("mpu6050 driver init\n");
    return 0;
}

static void __exit mpu6050_exit(void)
{
    i2c_del_driver(&mpu6050_driver);
    class_destroy(mpu6050_class);
    pr_info("mpu6050 driver exit\n");
}

module_init(mpu6050_init);
module_exit(mpu6050_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("XLP");
MODULE_DESCRIPTION("MPU6050 driver");
