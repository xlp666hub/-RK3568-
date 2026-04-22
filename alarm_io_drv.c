#include <linux/module.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/gpio/consumer.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/slab.h>

#define DEVICE_NAME "alarm_io"
#define CLASS_NAME  "alarm_io_class"
#define CMD_LEN     32

struct alarm_io_dev {
    struct device *dev;//打印日志要用

    struct gpio_desc *blue_gpio;
    struct gpio_desc *red_gpio;
    struct gpio_desc *buzzer_gpio;

    dev_t devt;
    struct cdev cdev;//要通过这个找到整个设备对象
    struct device *cdevice;
};

static struct class *alarm_io_class;

static int alarm_io_open(struct inode *inode, struct file *file)
{
    struct alarm_io_dev *alarm;

	//container_of三个参数
	//第一个参数：指向结构体中某个成员的指针
	//第二个参数：最终要得到的结构体类型
	//第三个参数：第一个参数的指针指向的成员在结构体中的名字
    alarm = container_of(inode->i_cdev, struct alarm_io_dev, cdev);
    file->private_data = alarm;

    return 0;
}

static int alarm_io_release(struct inode *inode, struct file *file)
{
    return 0;
}

static ssize_t alarm_io_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
    struct alarm_io_dev *alarm = file->private_data;
    char kbuf[CMD_LEN];
    char name[16];
    int value;
    int ret;

    if (count == 0)
        return 0;

    if (count >= CMD_LEN)
        count = CMD_LEN - 1;

    if (copy_from_user(kbuf, buf, count))
        return -EFAULT;

    kbuf[count] = '\0';

    /*
     * 用户态写入格式：
     * "blue 1"
     * "blue 0"
     * "red 1"
     * "buzzer 1"
     */

	//读字符串读到空格，制表符，或者换行符都会跳过，进行下一个新项目
    ret = sscanf(kbuf, "%15s %d", name, &value);//返回值ret代表解析出了几个项目
    if (ret != 2)
        return -EINVAL;

    value = !!value;//规范

	//当strcmp比较的两个字符串相同时，返回0
    if (!strcmp(name, "blue"))
	{
        gpiod_set_value_cansleep(alarm->blue_gpio, value);
    } 
	else if (!strcmp(name, "red")) 
	{
        gpiod_set_value_cansleep(alarm->red_gpio, value);
    } 
	else if (!strcmp(name, "buzzer")) 
	{
        gpiod_set_value_cansleep(alarm->buzzer_gpio, value);
    } 
	else 
	{
        return -EINVAL;
    }

    return count;
}

static const struct file_operations alarm_io_fops = {
    .owner   = THIS_MODULE,
    .open    = alarm_io_open,
    .release = alarm_io_release,
    .write   = alarm_io_write,
};

static int alarm_io_chrdev_register(struct alarm_io_dev *alarm)
{
    int ret;

    ret = alloc_chrdev_region(&alarm->devt, 0, 1, DEVICE_NAME);
    if (ret) 
	{
        dev_err(alarm->dev, "alloc_chrdev_region failed\n");
        return ret;
    }

    cdev_init(&alarm->cdev, &alarm_io_fops);
    alarm->cdev.owner = THIS_MODULE;

    ret = cdev_add(&alarm->cdev, alarm->devt, 1);
    if (ret) 
	{
        dev_err(alarm->dev, "cdev_add failed\n");
        unregister_chrdev_region(alarm->devt, 1);
        return ret;
    }

    alarm->cdevice = device_create(alarm_io_class, alarm->dev, alarm->devt, NULL, DEVICE_NAME);
    if (IS_ERR(alarm->cdevice)) 
	{
        ret = PTR_ERR(alarm->cdevice);
        dev_err(alarm->dev, "device_create failed\n");
        cdev_del(&alarm->cdev);
        unregister_chrdev_region(alarm->devt, 1);
        return ret;
    }

    return 0;
}

static void alarm_io_chrdev_unregister(struct alarm_io_dev *alarm)
{
    device_destroy(alarm_io_class, alarm->devt);
    cdev_del(&alarm->cdev);
    unregister_chrdev_region(alarm->devt, 1);
}

static int alarm_io_probe(struct platform_device *pdev)
{
    int ret;
    struct alarm_io_dev *alarm;

    alarm = devm_kzalloc(&pdev->dev, sizeof(*alarm), GFP_KERNEL);
    if (!alarm)
        return -ENOMEM;

    alarm->dev = &pdev->dev;

    alarm->blue_gpio = devm_gpiod_get(&pdev->dev, "blue", GPIOD_OUT_LOW);
    if (IS_ERR(alarm->blue_gpio)) 
	{
        dev_err(&pdev->dev, "failed to get blue gpio\n");
        return PTR_ERR(alarm->blue_gpio);
    }

    alarm->red_gpio = devm_gpiod_get(&pdev->dev, "red", GPIOD_OUT_LOW);
    if (IS_ERR(alarm->red_gpio)) 
	{
        dev_err(&pdev->dev, "failed to get red gpio\n");
        return PTR_ERR(alarm->red_gpio);
    }

    alarm->buzzer_gpio = devm_gpiod_get(&pdev->dev, "buzzer", GPIOD_OUT_LOW);
    if (IS_ERR(alarm->buzzer_gpio)) 
	{
        dev_err(&pdev->dev, "failed to get buzzer gpio\n");
        return PTR_ERR(alarm->buzzer_gpio);
    }

    ret = alarm_io_chrdev_register(alarm);
    if (ret)
        return ret;

    platform_set_drvdata(pdev, alarm);

    dev_info(&pdev->dev, "alarm_io probe success\n");
    return 0;
}

static int alarm_io_remove(struct platform_device *pdev)
{
    struct alarm_io_dev *alarm = platform_get_drvdata(pdev);

    alarm_io_chrdev_unregister(alarm);

    dev_info(&pdev->dev, "alarm_io remove success\n");
    return 0;
}

static const struct of_device_id alarm_io_of_match[] = {
    { .compatible = "xlp,alarm-io" },
    { }
};
MODULE_DEVICE_TABLE(of, alarm_io_of_match);

static struct platform_driver alarm_io_driver = {
    .probe  = alarm_io_probe,
    .remove = alarm_io_remove,
    .driver = {
        .name = "alarm_io",
        .of_match_table = alarm_io_of_match,
    },
};

static int __init alarm_io_init(void)
{
    int ret;

    alarm_io_class = class_create(THIS_MODULE, CLASS_NAME);
    if (IS_ERR(alarm_io_class))
        return PTR_ERR(alarm_io_class);

    ret = platform_driver_register(&alarm_io_driver);
    if (ret) {
        class_destroy(alarm_io_class);
        return ret;
    }

    pr_info("alarm_io driver init\n");
    return 0;
}

static void __exit alarm_io_exit(void)
{
    platform_driver_unregister(&alarm_io_driver);
    class_destroy(alarm_io_class);
    pr_info("alarm_io driver exit\n");
}

module_init(alarm_io_init);
module_exit(alarm_io_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("xlp");
MODULE_DESCRIPTION("standard char driver for blue led, red led and buzzer");
