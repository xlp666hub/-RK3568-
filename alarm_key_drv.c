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
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/delay.h>

#define DEVICE_NAME "alarm_key"
#define CLASS_NAME  "alarm_key_class"

struct alarm_key_dev {
    struct device *dev;
    struct gpio_desc *key_gpio;//按键对应GPIO
    int irq;//按键对应中断号

    int key_value;   //用户态读到的事件值
    int event_pending;   //是否有未读事件 

    unsigned int debounce_ms;//消抖时间

    wait_queue_head_t wq; //等待队列
    struct mutex lock; //互斥锁保证同一时刻只有一个执行流能进入临界区

	//字符设备相关
    dev_t devt;
    struct cdev cdev;
    struct device *cdevice;
};

static struct class *alarm_key_class;

static irqreturn_t alarm_key_thread_fn(int irq, void *dev_id)
{
    struct alarm_key_dev *keydev = dev_id;
    int value;

    msleep(keydev->debounce_ms);

    /*
     * 设备树里key-gpios配的是GPIO_ACTIVE_LOW，
     * 所以这里读到1表示逻辑上按下
     * 读到0表示逻辑上松开
     */
    value = gpiod_get_value_cansleep(keydev->key_gpio);
    if (value == 1) 
	{
        mutex_lock(&keydev->lock);
        keydev->key_value = 1;
        keydev->event_pending = 1;
        mutex_unlock(&keydev->lock);

        wake_up_interruptible(&keydev->wq);
        dev_info(keydev->dev, "key event detected\n");
    }

    return IRQ_HANDLED;
}

static int alarm_key_open(struct inode *inode, struct file *file)
{
    struct alarm_key_dev *keydev;

    keydev = container_of(inode->i_cdev, struct alarm_key_dev, cdev);
    file->private_data = keydev;

    return 0;
}

static int alarm_key_release(struct inode *inode, struct file *file)
{
    return 0;
}

static ssize_t alarm_key_read(struct file *file, char __user *buf,
                              size_t count, loff_t *ppos)
{
    struct alarm_key_dev *keydev = file->private_data;
    int value;
	int ret;

    if (count < sizeof(int))
        return -EINVAL;

    if (file->f_flags & O_NONBLOCK) 
	{
        if (!keydev->event_pending)
            return -EAGAIN;
    } 
	else 
	{
        ret = wait_event_interruptible(keydev->wq, keydev->event_pending);
        if (ret)
            return ret;
    }

    mutex_lock(&keydev->lock);
    value = keydev->key_value;
    keydev->key_value = 0;
    keydev->event_pending = 0;
    mutex_unlock(&keydev->lock);

    if (copy_to_user(buf, &value, sizeof(value)))
        return -EFAULT;

    return sizeof(value);
}

static __poll_t alarm_key_poll(struct file *file, poll_table *wait)
{
    struct alarm_key_dev *keydev = file->private_data;
    __poll_t mask = 0;

    poll_wait(file, &keydev->wq, wait);

    if (keydev->event_pending)
        mask |= POLLIN | POLLRDNORM;

    return mask;
}

static const struct file_operations alarm_key_fops = {
    .owner   = THIS_MODULE,
    .open    = alarm_key_open,
    .release = alarm_key_release,
    .read    = alarm_key_read,
	.poll    = alarm_key_poll,
};

static int alarm_key_chrdev_register(struct alarm_key_dev *keydev)
{
    int ret;

    ret = alloc_chrdev_region(&keydev->devt, 0, 1, DEVICE_NAME);
    if (ret) {
        dev_err(keydev->dev, "alloc_chrdev_region failed\n");
        return ret;
    }

    cdev_init(&keydev->cdev, &alarm_key_fops);
    keydev->cdev.owner = THIS_MODULE;

    ret = cdev_add(&keydev->cdev, keydev->devt, 1);
    if (ret) {
        dev_err(keydev->dev, "cdev_add failed\n");
        unregister_chrdev_region(keydev->devt, 1);
        return ret;
    }

    keydev->cdevice = device_create(alarm_key_class, keydev->dev,
                                    keydev->devt, NULL, DEVICE_NAME);
    if (IS_ERR(keydev->cdevice)) {
        ret = PTR_ERR(keydev->cdevice);
        dev_err(keydev->dev, "device_create failed\n");
        cdev_del(&keydev->cdev);
        unregister_chrdev_region(keydev->devt, 1);
        return ret;
    }

    return 0;
}

static void alarm_key_chrdev_unregister(struct alarm_key_dev *keydev)
{
    device_destroy(alarm_key_class, keydev->devt);
    cdev_del(&keydev->cdev);
    unregister_chrdev_region(keydev->devt, 1);
}

static int alarm_key_probe(struct platform_device *pdev)
{
    int ret;
    struct alarm_key_dev *keydev;

    keydev = devm_kzalloc(&pdev->dev, sizeof(*keydev), GFP_KERNEL);
    if (!keydev)
        return -ENOMEM;

    keydev->dev = &pdev->dev;

	keydev->key_value = 0;
	keydev->event_pending = 0;
	keydev->debounce_ms = 20;

	init_waitqueue_head(&keydev->wq);
	mutex_init(&keydev->lock);

    keydev->key_gpio = devm_gpiod_get(&pdev->dev, "key", GPIOD_IN);
    if (IS_ERR(keydev->key_gpio)) 
	{
        dev_err(&pdev->dev, "failed to get key gpio\n");
        return PTR_ERR(keydev->key_gpio);
    }

    keydev->irq = gpiod_to_irq(keydev->key_gpio);
    if (keydev->irq < 0) 
	{
        dev_err(&pdev->dev, "failed to get irq\n");
        return keydev->irq;
    }

    ret = devm_request_threaded_irq(&pdev->dev,
                                keydev->irq,
                                NULL,
                                alarm_key_thread_fn,
                                IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
                                "alarm_key_irq",
                                keydev);
    if (ret)
	{
        dev_err(&pdev->dev, "request irq failed\n");
        return ret;
    }

    ret = alarm_key_chrdev_register(keydev);
    if (ret)
        return ret;

    platform_set_drvdata(pdev, keydev);

    dev_info(&pdev->dev, "alarm_key probe success\n");
    return 0;
}

static int alarm_key_remove(struct platform_device *pdev)
{
    struct alarm_key_dev *keydev = platform_get_drvdata(pdev);

    alarm_key_chrdev_unregister(keydev);
    dev_info(&pdev->dev, "alarm_key remove success\n");
    return 0;
}

static const struct of_device_id alarm_key_of_match[] = {
    { .compatible = "xlp,alarm-key" },
    { }
};
MODULE_DEVICE_TABLE(of, alarm_key_of_match);

static struct platform_driver alarm_key_driver = {
    .probe  = alarm_key_probe,
    .remove = alarm_key_remove,
    .driver = {
        .name = "alarm_key",
        .of_match_table = alarm_key_of_match,
    },
};

static int __init alarm_key_init(void)
{
    int ret;

    alarm_key_class = class_create(THIS_MODULE, CLASS_NAME);
    if (IS_ERR(alarm_key_class))
        return PTR_ERR(alarm_key_class);

    ret = platform_driver_register(&alarm_key_driver);
    if (ret) {
        class_destroy(alarm_key_class);
        return ret;
    }

    pr_info("alarm_key driver init\n");
    return 0;
}

static void __exit alarm_key_exit(void)
{
    platform_driver_unregister(&alarm_key_driver);
    class_destroy(alarm_key_class);
    pr_info("alarm_key driver exit\n");
}

module_init(alarm_key_init);
module_exit(alarm_key_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("xlp");
MODULE_DESCRIPTION("alarm key driver");
