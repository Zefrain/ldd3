#include <linux/init.h>
#include <linux/module.h>
#include "scull.h"
#include <linux/slab.h>

MODULE_LICENSE("Dual BSD/GPL");

#define SCULL_MAJOR   0
#define SCULL_MINOR   0
#define SCULL_N_DEVS  4
#define SCULL_QUANTUM 2000
#define SCULL_QSET    1000

int scull_major = SCULL_MAJOR;
int scull_minor = SCULL_MINOR;
int scull_quantum = SCULL_QUANTUM;
int scull_qset =    SCULL_QSET;
int scull_n_devs = SCULL_N_DEVS;

struct scull_dev *devs;

struct file_operations scull_fops = {
    .owner = THIS_MODULE,
    /* .llseek = scull_llseek, */
    .read = scull_read,
    .write = scull_write,
    /* .ioctl = scull_ioctl, */
    .open = scull_open,
    .release = scull_release,
};


int scull_release(struct inode *inode, struct file *filp)
{
	return 0;
}

static void scull_setup_cdev(struct scull_dev *dev, int index)
{
	int err;
	int devno = MKDEV(scull_major, scull_minor + index);

	cdev_init(&dev->cdev, &scull_fops);
	dev->cdev.owner = THIS_MODULE;
	dev->cdev.ops = &scull_fops;
	err = cdev_add(&dev->cdev, devno, 1);
	if (err) {
		printk(KERN_NOTICE "Error %d adding scull%d", err, index);
	}
}

int scull_trim(struct scull_dev *dev)
{
	struct scull_qset *next, *dptr;
	int		   qset = dev->qset;

	int i;
	for (dptr = dev->data; dptr; dptr = next) {
		if (dptr->data) {
			for (i = 0; i < qset; i++) {
				kfree(dptr->data[i]);
			}
			kfree(dptr->data);
			dptr->data = NULL;
		}
		next = dptr->next;
		kfree(dptr);
	}

	dev->size = 0;
	dev->quantum = scull_quantum;
	dev->qset = scull_qset;
	dev->data = NULL;
	return 0;
}

int scull_open(struct inode *inode, struct file *filp)
{
	struct scull_dev *dev;

	dev = container_of(inode->i_cdev, struct scull_dev, cdev);
	filp->private_data = dev;

	if ((filp->f_flags & O_ACCMODE) == O_WRONLY) {
		scull_trim(dev);
	}

	return 0;
}

static struct scull_qset *scull_follow(struct scull_dev *dev, int item)
{
	struct scull_qset *dptr = dev->data;
	int	    i = 0;

	if (!dptr) {
		dptr = kmalloc(sizeof(struct scull_qset), GFP_KERNEL);
		if(!dptr) {
			printk(KERN_ERR "kmalloc error");
			goto out;
		}
		memset(dptr, 0, sizeof(struct scull_qset));
	}

	for (i = 0; i < item; ++i) {
		dptr = dptr->next;
		if (!dptr->next) {
			dptr->next =
			    kmalloc(sizeof(struct scull_qset), GFP_KERNEL);
			if (dptr->next == NULL)
				goto out;
			memset(dptr->next, 0, sizeof(struct scull_qset));
		}
		dptr = dptr->next;
	}

out:
	return dptr;
}

ssize_t
scull_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
	struct scull_dev	 *dev = filp->private_data;
	struct scull_qset *dptr; /* the first listitem */
	int		   quantum, qset;
	int		   itemsize; /* how many bytes in the listitem */
	int		   item, s_pos, q_pos, rest;
	ssize_t		   retval = 0;
	if (down_interruptible(&dev->sem))
		return -ERESTARTSYS;
	quantum = dev->quantum;
	qset = dev->qset;
	itemsize = quantum * qset;
	if (*f_pos >= dev->size)
		goto out;
	if (*f_pos + count > dev->size)
		count = dev->size - *f_pos;
	/* find listitem, qset index, and offset in the quantum */
	item = (long)*f_pos / itemsize;
	rest = (long)*f_pos % itemsize;
	s_pos = rest / quantum;
	q_pos = rest % quantum;
	/* follow the list up to the right position (defined elsewhere) */
	dptr = scull_follow(dev, item);
	if (dptr == NULL || !dptr->data || !dptr->data[s_pos])
		goto out; /* don't fill holes */
	/* read only up to the end of this quantum */
	if (count > quantum - q_pos)
		count = quantum - q_pos;
	if (copy_to_user(buf, dptr->data[s_pos] + q_pos, count)) {
		retval = -EFAULT;
		goto out;
	}
	*f_pos += count;
	retval = count;
out:
	up(&dev->sem);
	return retval;
}

ssize_t scull_write(struct file	*filp,
		    const char __user *buf,
		    size_t count,
		    loff_t *f_pos)
{
	struct scull_dev	 *dev = filp->private_data;
	struct scull_qset *dptr = NULL;
	int		   quantum = dev->quantum, qset = dev->qset;
	int		   itemsize = quantum * qset;
	int		   item, s_pos, q_pos, rest;
	ssize_t retval = -ENOMEM; /* value used in "goto out" statements */

	if (down_interruptible(&dev->sem))
		return -ERESTARTSYS;
	/* find listitem, qset index and offset in the quantum */
	item = (long)*f_pos / itemsize;
	rest = (long)*f_pos % itemsize;
	s_pos = rest / quantum;
	q_pos = rest % quantum;
	/* follow the list up to the right position */
	dptr = scull_follow(dev, item);
	if (dptr == NULL)
		goto out;
	if (!dptr->data) {
		dptr->data = kmalloc(qset * sizeof(char *), GFP_KERNEL);
		if (!dptr->data)
			goto out;
		memset(dptr->data, 0, qset * sizeof(char *));
	}
	if (!dptr->data[s_pos]) {
		dptr->data[s_pos] = kmalloc(quantum, GFP_KERNEL);
		if (!dptr->data[s_pos])
			goto out;
	}
	/* write only up to the end of this quantum */
	if (count > quantum - q_pos)
		count = quantum - q_pos;
	if (copy_from_user(dptr->data[s_pos] + q_pos, buf, count)) {
		retval = -EFAULT;
		goto out;
	}
	*f_pos += count;
	retval = count;
	/* update the size */
	if (dev->size < *f_pos)
		dev->size = *f_pos;
out:
	up(&dev->sem);
	return retval;
}

static void scull_exit(void)
{
	int i = 0;
	dev_t devno = 0;
	if (devs) {
		for (i = 0; i < scull_n_devs; i++) {
			scull_trim(devs + i);
			cdev_del(&devs[i].cdev);
		}
		kfree(devs);
	}

	unregister_chrdev_region(devno, scull_n_devs);
}

static int scull_init(void)
{
	int i = 0;
	int ret = 0;
	dev_t devno = MKDEV(scull_major, scull_minor);

	if (scull_major) {
		devno = MKDEV(scull_major, scull_minor);
		ret = register_chrdev_region(devno, scull_n_devs, "scull");
	} else {
		ret = alloc_chrdev_region(&devno, scull_minor, scull_n_devs,
				"scull");
		scull_major = MAJOR(devno);
	}

	if (ret < 0) {
		printk(KERN_WARNING "scull cannot get major");
		goto fail;
	}

	devs = kmalloc(scull_n_devs * sizeof(struct scull_dev), GFP_KERNEL);
	if (!devs) {
		ret = -ENOMEM;
		goto fail;  /* Make this more graceful */
	}

	memset(devs, 0, scull_n_devs * sizeof(struct scull_dev));
	for (i = 0; i < scull_n_devs; i++) {
		devs[i].quantum = scull_quantum;
		devs[i].qset = scull_qset;
		scull_setup_cdev(&devs[i], i);
	}

	return 0;

fail:
	scull_exit();
	return ret;
}


module_init(scull_init);
module_exit(scull_exit);
