#include <linux/kernel.h> //print functions
#include <linux/init.h> //for __init, see code
#include <linux/module.h> //for module init and exit macros
#include <linux/notifier.h> //For notification struct types
#include <linux/of.h> //For device tree struct types


//I don't know why, but for some reason <linux/of.h> isn't defining these?

/* For updating the device tree at runtime */
#define OF_RECONFIG_ATTACH_NODE		0x0001
#define OF_RECONFIG_DETACH_NODE		0x0002
#define OF_RECONFIG_ADD_PROPERTY	0x0003
#define OF_RECONFIG_REMOVE_PROPERTY	0x0004
#define OF_RECONFIG_UPDATE_PROPERTY	0x0005

static void print_property(struct property *p) {
	char line[160];
	int pos = 0;
	int tmp;
	
	if (p == NULL) return;
	if (p->name == NULL) {
		tmp = sprintf(line+pos, "%s", "((anonymous property)) = 0x");
		pos += tmp;
	} else {
		tmp = sprintf(line+pos, "%s = 0x", p->name);
		pos += tmp;
	}
	
	if (p->length <= 0 || p->value == NULL) {
		sprintf(line+pos, "((empty))");
	} else {
		unsigned char *data = (unsigned char *) p->value;
		int space_left = sizeof(line) - pos - 2; 
		//Room for null, and safety against off-by-one errors I'm too 
		//lazy to figure out
		
		int stop_point = p->length;
		int add_dots = 0;
		int i;
		//Each byte will make two hex digits. If we don't have enough
		//room for all of them, we'll print as many as we can and end
		//with "..."
		if ((p->length << 1) > space_left) {
			stop_point = (space_left-3)>>1;
			add_dots = 1;
		}
		
		for (i = 0; i < stop_point; i++) {
			sprintf(line+pos, "%02x", (+data[i]) & 0xFF);
			pos += 2;
		}
		
		if (add_dots) {
			line[pos++] = '.';
			line[pos++] = '.';
			line[pos++] = '.';
			line[pos] = 0;
		}
	}
	
	printk(KERN_ALERT "\t\t%s\n", line);
}

static void print_device_node(struct device_node *dn, int levels) {
	//struct property *cur = dn->properties;
	if (levels < 0) return;
	if (dn == NULL) {
		printk(KERN_ALERT "\t((empty device node))\n");
		return;
	}
	printk(KERN_ALERT "\t\t.name = %s\n", dn->name);
	printk(KERN_ALERT "\t\t.full_name = %s\n", dn->full_name);
	/*
	while (cur != NULL) {
		print_property(cur);
		cur = cur->next;
	}*/
	
	if (levels > 1) {
		printk(KERN_ALERT "\nChild of:\n");
		print_device_node(dn->parent, levels - 1);
	}
}

static int print_dt_updates(struct notifier_block *nb, unsigned long action, void *arg) {
	//Add nicer names to the stuff we were given
	struct of_reconfig_data *rd = (struct of_reconfig_data *) arg;
	struct device_node *dn = rd->dn;
	struct property *newp = rd->prop;
	struct property *oldp = rd->old_prop;
	
	printk(KERN_ALERT "Received a device tree update:\n");
	switch (action) {
		case OF_RECONFIG_ATTACH_NODE:
			printk(KERN_ALERT "\tATTACH NODE\n");
			print_device_node(dn, 1);
			break;
		case OF_RECONFIG_DETACH_NODE     :
			printk(KERN_ALERT "\tDETACH NODE\n");
			print_device_node(dn, 1);
			break;
		case OF_RECONFIG_ADD_PROPERTY    :
			printk(KERN_ALERT "\tADD PROPERTY\n");
			print_property(newp);
			printk(KERN_ALERT "\tOF\n");
			print_device_node(dn, 1);
			break;
		case OF_RECONFIG_REMOVE_PROPERTY :
			printk(KERN_ALERT "\tREMOVE PROPERTY\n");
			print_property(newp);
			printk(KERN_ALERT "\tOF\n");
			print_device_node(dn, 1);
			break;
		case OF_RECONFIG_UPDATE_PROPERTY :
			printk(KERN_ALERT "\tUPDATE PROPERTY\n");
			print_property(newp);
			printk(KERN_ALERT "\tOF\n");
			print_device_node(dn, 1);
			printk(KERN_ALERT "\tWHICH USED TO BE\n");
			print_property(oldp);
			break;
		default:
			printk(KERN_ERR "\tUNRECOGNIZED DEVICE TREE UPDATE CODE%ld\n", action);
			break;
	}
	
	return 0;
}

static struct notifier_block dtoprinter_notifier = {
	.notifier_call = print_dt_updates,
};

int registered = 0;

static int __init our_init(void) { 
	
	int err = of_reconfig_notifier_register(&dtoprinter_notifier);
	if (err < 0) {
		printk(KERN_ERR "Could not register notifier\n");
	} else {
		printk(KERN_ALERT "DTO printer inserted!\n"); 
		registered = 1;
	}
	
	return err; //Propagate error code
} 

static void our_exit(void) { 
	if (registered) of_reconfig_notifier_unregister(&dtoprinter_notifier);
	
	printk(KERN_ALERT "DTO printer removed!\n"); 
} 

MODULE_LICENSE("Dual BSD/GPL"); 

module_init(our_init); 
module_exit(our_exit);
