/*

Driver originally written by Clark and Camilo.

(Nov12, 2019) Marco found some stuff online and cleaned up the code.
* See samples/kobject/kobject-example.c in the kernel source tree.
* (https://elixir.bootlin.com/linux/latest/source/samples/kobject/kobject-example.c)

*/

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kdev_t.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/sysfs.h> 
#include <linux/kobject.h> 
#include <asm/io.h>

#define CLK_BASE 0xFF5E0000U
#define PLCLK0 0xC0
#define PLCLK1 0xC4
#define PLCLK2 0xC8
#define PLCLK3 0xCC

#define ENABLE_MASK 0x01000000U
#define ENABLE_MASK_INV 0xFEFFFFFFU
#define ENABLE_AWAYS_TRUE 0x00010002U
#define ENABLE_AWAYS_TRUE_INV 0xFFC0FFFDU
#define FREQ_MASK 0x00003F00U
#define FREQ_MASK_INV 0xFFFFFC0FFU

int base_freq = 800;

struct plclk {
    struct kobject *kobj;
    int hex;
    int freq;
    int ena;
    void *clk_virt;
    
    struct kobj_attribute attr_ena;
    struct kobj_attribute attr_freq;
};

int freq_find(int freq) {
	int minIndex = 32;
	int min = 9999;
	int i;
	for (i = 1; i <= 32; i++) {
		if (min > abs(base_freq/i-freq)) {
			min = abs(base_freq/i-freq);
			minIndex = i;
		}
	}
	return minIndex;
}

static ssize_t ena_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    struct plclk *clk = container_of(attr, struct plclk, attr_ena);
	return sprintf(buf, "%d\n", clk->ena);
}

static ssize_t ena_store(struct kobject *kobj, struct kobj_attribute *attr,const char *buf, size_t count)
{
    struct plclk *clk = container_of(attr, struct plclk, attr_ena);
	int enatmp;
	if (sscanf(buf,"%d",&enatmp) != 1) {
		printk(KERN_ERR "it is illegal to write non-numeric value to mpsoc regs!\n");
		return count;
	}
    
	clk->ena = enatmp ? 1 : 0;
	clk->hex = (((clk->hex & ENABLE_MASK_INV) | ((clk->ena << 24) & ENABLE_MASK)) & ENABLE_AWAYS_TRUE_INV) | ENABLE_AWAYS_TRUE;
	if (clk->ena) {
		printk(KERN_INFO "enabling %s\n", kobj->name);
	} else {
		printk(KERN_INFO "disabling %s\n", kobj->name);
	}
	writel(clk->hex,clk->clk_virt);
	return count;
}

static ssize_t freq_show /* "freak show" */ (struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    struct plclk *clk = container_of(attr, struct plclk, attr_freq);
	return sprintf(buf, "%d MHz\n", clk->freq);
}

static ssize_t freq_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
    struct plclk *clk = container_of(attr, struct plclk, attr_freq);
	int freqtmp;
	int divider;
	if (sscanf(buf,"%d",&freqtmp) != 1) {
		printk(KERN_ERR "it is illegal to write non-numeric value to mpsoc regs!\n");
		return count;
	}
	divider = freq_find(freqtmp);
	clk->freq = base_freq / divider;
	printk(KERN_INFO "changing %s's frequency to %d\n", kobj->name, clk->freq);
	clk->hex = (clk->hex & FREQ_MASK_INV) | ((divider << 8) & FREQ_MASK);
	writel(clk->hex, clk->clk_virt);
	return count;
}

static struct plclk pl_clocks[4];
static struct kobject *mpsoc_root, *clocks;

static int __init mpsoc_psregs_init(void) {
    int i;
	mpsoc_root = kobject_create_and_add("mpsoc",NULL);
	clocks = kobject_create_and_add("clocks",mpsoc_root); 
    
    pl_clocks[0].clk_virt = ioremap_nocache(CLK_BASE+PLCLK0,4);
    pl_clocks[1].clk_virt = ioremap_nocache(CLK_BASE+PLCLK1,4);
    pl_clocks[2].clk_virt = ioremap_nocache(CLK_BASE+PLCLK2,4);
    pl_clocks[3].clk_virt = ioremap_nocache(CLK_BASE+PLCLK3,4);
    
    for (i = 0; i < 4; i++) {        
        int retval;
        char name[8];
        sprintf(name, "fclk%d", i);
        pl_clocks[i].kobj = kobject_create_and_add(name, clocks);
        
        pl_clocks[i].hex = readl(pl_clocks[i].clk_virt);
        pl_clocks[i].ena = pl_clocks[i].hex >> 24;
        pl_clocks[i].freq = base_freq / ((pl_clocks[i].hex & FREQ_MASK) >> 8);
        
        pl_clocks[i].attr_ena.attr.name = "enable";
        pl_clocks[i].attr_ena.attr.mode = 0664;
        pl_clocks[i].attr_ena.show = ena_show;
        pl_clocks[i].attr_ena.store = ena_store;
        
        pl_clocks[i].attr_freq.attr.name = "frequency";
        pl_clocks[i].attr_freq.attr.mode = 0664;
        pl_clocks[i].attr_freq.show = freq_show;
        pl_clocks[i].attr_freq.store = freq_store;
        
        retval = sysfs_create_file(pl_clocks[i].kobj, &(pl_clocks[i].attr_ena.attr));
        if (retval) {
            printk(KERN_ERR "Everything is now broken! Please reboot!");
            //The error-handling code is messy enough that I don't want to write
            //it. The user will have to reboot.
            return retval;
        }
        
        retval = sysfs_create_file(pl_clocks[i].kobj, &(pl_clocks[i].attr_freq.attr));
        if (retval) {
            printk(KERN_ERR "Everything is now broken! Please reboot!");
            return retval;
        }
    }

	printk(KERN_INFO "Finished registering MPSOC sysfs register file group!\n");
	return 0;
}
 
void __exit mpsoc_psregs_exit(void)
{
    int i;
    for (i = 0; i < 4; i++)	{
        iounmap(pl_clocks[i].clk_virt);
        kobject_put(pl_clocks[i].kobj);
	}
	kobject_put(clocks);
	kobject_put(mpsoc_root);
	printk(KERN_INFO "Finished unregistering MPSOC sysfs register file group!\n");
}
 
module_init(mpsoc_psregs_init);
module_exit(mpsoc_psregs_exit);
 
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Clark Shen <qianfeng.shen@gmail.com>");
MODULE_DESCRIPTION("MPSOC PS CONFIGURATION REGS driver");
MODULE_VERSION("0");
