#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#include "scull.h"

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
int scull_nr_devs = SCULL_N_DEVS;

struct scull_dev *scull_devices;

static struct file_operations scull_fops = {
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

int scull_read_procmem(
    char *buf, char **start, off_t offset, int count, int *eof, void *data)
{
	int i, j, len = 0;
	int limit = count - 80;

	for (i = 0; i < scull_nr_devs && len <= limit; i++) {
		struct scull_dev  *d = &scull_devices[i];
		struct scull_qset *qs = d->data;

		if (down_interruptible(&d->sem)) {
			return -ERESTARTSYS;
		}

		len += sprintf(buf + len,
			       "\nDevice %i: qset %i, q %i, sz %li\n",
			       i,
			       d->qset,
			       d->quantum,
			       d->size);

		if (qs->data && !qs->next) { // 只转储最后一页
			for (j = 0; j < d->qset; j++) {
				len += sprintf(buf + len,
					       "	% 4i: %8p\n",
					       j,
					       qs->data[j]);
			}
		}
		up(&scull_devices[i].sem);
	}

	*eof = 1;

	return len;
}

static void *scull_seq_start(struct seq_file *s, loff_t *pos)
{
	if (*pos >= scull_nr_devs) {
		return NULL;
	}
	return scull_devices + *pos;
}

static void *scull_seq_next(struct seq_file *s, void *v, loff_t *pos) 
{
	(*pos)++;

	return scull_seq_start(s, pos);
}

static void scull_seq_stop(struct seq_file *s, void *v)
{
	return;
}

static int scull_seq_show(struct seq_file *s, void *v)
{
	struct scull_dev	 *dev = (struct scull_dev *)v;
	struct scull_qset *d = NULL;
	int		   i = 0;

	if (down_interruptible(&dev->sem)) {
		return -ERESTARTSYS;
	}

	seq_printf(s,
		   "\nDevice %i: qset %i, q %i sz %li\n",
		   (int)(dev - scull_devices),
		   dev->qset,
		   dev->quantum,
		   dev->size);

	for (d = dev->data; d; d = d->next) {
		seq_printf(s, "	item at %p, qset at %p\n", d, d->data);

		if (d->data && !d->next) {
			for (i = 0; i < dev->qset; i++) {
				if (d->data[i]) {
					seq_printf(
					    s, "	%4i: %8p\n", i, d->data[i]);
				}
			}
		}
	}
	up(&dev->sem);
	return 0;
}

static struct seq_operations scull_seq_ops = {
	.start = scull_seq_start,
	.stop = scull_seq_stop,
	.next = scull_seq_next,
	.show = scull_seq_show
};

static int scull_proc_open(struct inode *inode,  struct file *file)
{
	return seq_open(file, &scull_seq_ops);
}

static struct proc_ops scull_proc_ops = {
	.proc_open = scull_proc_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = seq_release
};

static void scull_proc_create(void)
{
	struct proc_dir_entry *entry = NULL;
	entry = proc_create("scullseq", 0, NULL, &scull_proc_ops);
}

static void scull_exit(void)
{
	int i = 0;
	dev_t devno = 0;
	if (scull_devices) {
		for (i = 0; i < scull_nr_devs; i++) {
			scull_trim(scull_devices + i);
			cdev_del(&scull_devices[i].cdev);
		}
		kfree(scull_devices);
	}

	unregister_chrdev_region(devno, scull_nr_devs);
}

static int scull_init(void)
{
	int i = 0;
	int ret = 0;
	dev_t devno = MKDEV(scull_major, scull_minor);

	if (scull_major) {
		devno = MKDEV(scull_major, scull_minor);
		ret = register_chrdev_region(devno, scull_nr_devs, "scull");
	} else {
		ret = alloc_chrdev_region(&devno, scull_minor, scull_nr_devs,
				"scull");
		scull_major = MAJOR(devno);
	}

	if (ret < 0) {
		printk(KERN_WARNING "scull cannot get major");
		goto fail;
	}

	scull_devices = kmalloc(scull_nr_devs * sizeof(struct scull_dev), GFP_KERNEL);
	if (!scull_devices) {
		ret = -ENOMEM;
		goto fail;  /* Make this more graceful */
	}

	memset(scull_devices, 0, scull_nr_devs * sizeof(struct scull_dev));
	for (i = 0; i < scull_nr_devs; i++) {
		scull_devices[i].quantum = scull_quantum;
		scull_devices[i].qset = scull_qset;
		scull_setup_cdev(&scull_devices[i], i);
	}


	scull_proc_create();

	return 0;

fail:
	scull_exit();
	return ret;
}

module_init(scull_init);
module_exit(scull_exit);
