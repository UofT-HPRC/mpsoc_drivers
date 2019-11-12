#include <linux/types.h>
#include <linux/ioctl.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/poll.h>
#include <asm/uaccess.h>
#define VMEM_FLAGS (VM_IO | VM_DONTEXPAND | VM_DONTDUMP)
struct mpsoc_axireg_priv {
	wait_queue_head_t wq;
	unsigned int num_pages;
	char **page_ptr;
};

static int mpsoc_axireg_open(struct inode *inode, struct file *filp)
{
	struct mpsoc_axireg_priv *priv = (struct mpsoc_axireg_priv *) kzalloc(sizeof(struct mpsoc_axireg_priv), GFP_KERNEL);
	if (priv == NULL)
		return -ENOMEM;
	init_waitqueue_head(&priv->wq);
	filp->private_data = (void *) priv;
	printk(KERN_INFO "open MPSOC axi reg charactor device\n");
	return 0;
}

static int mpsoc_axireg_release(struct inode *inode, struct file *filp)
{
	struct mpsoc_axireg_priv *priv = (struct mpsoc_axireg_priv *) filp->private_data;
	unsigned int i;
	for (i = 0; i < priv->num_pages; i++)
		free_page((unsigned long) priv->page_ptr[i]);
	if (priv->page_ptr)
		kfree(priv->page_ptr);
	kfree(priv);
	printk(KERN_INFO "close MPSOC axi reg charactor device\n");
	return 0;
}

static int mpsoc_axireg_mmap(struct file *filp, struct vm_area_struct *vma)
{
	int rc;
	unsigned long off;
	unsigned long phys;
	unsigned long vsize;
	unsigned long psize;

	off = vma->vm_pgoff << PAGE_SHIFT;
	phys = 0xA0000000 + off;
	vsize = vma->vm_end - vma->vm_start;
	psize = vsize;
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

	vma->vm_flags |= VMEM_FLAGS;
	rc = io_remap_pfn_range(vma, vma->vm_start, phys >> PAGE_SHIFT,
		vsize, vma->vm_page_prot);
	if (rc) return -EINVAL;
	return 0;
}

static struct file_operations mpsoc_axireg_fops = {
	.owner = THIS_MODULE,
	.open = mpsoc_axireg_open,
	.mmap = mpsoc_axireg_mmap,
	.release = mpsoc_axireg_release,
};

struct miscdevice mpsoc_axireg_cdevsw = {
	MISC_DYNAMIC_MINOR,
	"mpsoc_axiregs",
	&mpsoc_axireg_fops,
};

static int __init mpsoc_axireg_module_init(void)
{
	mpsoc_axireg_cdevsw.mode = 0666;
	misc_register(&mpsoc_axireg_cdevsw);
	printk(KERN_INFO "MPSOC axi register driver is loaded\n");
	return 0;
}

static void __exit mpsoc_axireg_module_exit(void)
{
	misc_deregister(&mpsoc_axireg_cdevsw);
	printk(KERN_INFO "MPSOC axi register driver is unloaded\n");
}

module_init(mpsoc_axireg_module_init);
module_exit(mpsoc_axireg_module_exit);

MODULE_LICENSE("Dual BSD/GPL");
