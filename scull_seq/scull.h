#ifndef SCULL_H
#define SCULL_H

#include <linux/types.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/cdev.h>
#include <linux/kernel.h>

struct scull_qset {
	void	     **data;
	struct scull_qset *next;
};

struct scull_dev {
	struct scull_qset *data;
	int		   quantum;
	int		   qset;
	unsigned long	   size;
	unsigned int	   access_key;
	struct semaphore   sem;
	struct cdev	   cdev;
};

int	    scull_release(struct inode *inode, struct file *filp);
static void scull_setup_cdev(struct scull_dev *dev, int index);
int	    scull_trim(struct scull_dev *dev);
int	    scull_open(struct inode *inode, struct file *filp);
ssize_t
scull_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos);

ssize_t scull_write(struct file	*filp,
		    const char __user *buf,
		    size_t count,
		    loff_t *f_pos);

int scull_read_procmem(
    char *buf, char **start, off_t offset, int count, int *eof, void *data);

#endif // SCULL_H
