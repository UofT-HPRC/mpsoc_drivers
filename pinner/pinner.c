#include <linux/fs.h> //struct file, struct file_operations
#include <linux/init.h> //for __init, see code
#include <linux/module.h> //for module init and exit macros
#include <linux/miscdevice.h> //for misc_device_register and struct micdev
#include <linux/uaccess.h> //For copy_to_user and copy_from_user
#include <linux/mutex.h> //For mutexes
#include <asm/page.h> //For PAGE_SHIFT
#include <linux/mm.h> //For find_vma
#include <linux/random.h> //For get_random_bytes
#include <linux/list.h> //For linked lists
#include <linux/slab.h> //For kzalloc, kfree
#include <linux/stddef.h> //For offsetof
#include <asm/cacheflush.h> //For flush_cache_range
#include "pinner.h" //Custom data types and defines shared with userspace
#include "pinner_private.h" //Private custom data types and macros

static DEFINE_MUTEX(users_mutex);
static LIST_HEAD(users);

static void put_page_list(struct page **p, int num_pages) {
    int i;
    //printk(KERN_ALERT "Entered put_page_list\n");
    for (i = 0; i < num_pages; i++) {
        //TODO: do we have to manually mark them as dirty?
        put_page(p[i]);
    }
}

static void pinner_free_pinning(struct pinning *p) {
    //printk(KERN_ALERT "Entered pinner_free_pinning\n");
    //put pages
    put_page_list(p->pages, p->num_pages);
    
    //free page pointers
    kfree(p->pages);
    
    //Remove pinning from list
    list_del(&(p->list));
    
    //Free pinning struct
    kfree(p);
}

static void pinner_free_pinnings(struct proc_info *info) {
    //printk(KERN_ALERT "Entered pinner_free_pinnings\n");
    //Iterate through the list of pinnings inside this proc_info struct
    //and free them all
    while (!list_empty(&(info->pinning_list))) {
        struct pinning *p = list_entry(info->pinning_list.next, struct pinning, list);
        pinner_free_pinning(p);
    }
}

static void pinner_free_proc_info(struct proc_info *info) {
    //printk(KERN_ALERT "Entered pinner_free_proc_info\n");
    //Free all the pinnings stored in this proc_info struct
    pinner_free_pinnings(info);
    
    //Remove from the list
    mutex_lock(&users_mutex); //Need to watch out for race conditions
    list_del(&(info->list));
    mutex_unlock(&users_mutex);
    
    //Free the struct itself
    kfree(info);
}

static int pinner_send_physlist(struct pinner_cmd *cmd, struct pinning *p, //assumption: p->num_pages > 0
                                unsigned long first_page_offset, unsigned total_sz)
{
    int ret = 0;
    int n;
    int i;
    struct pinner_physlist_entry *entries = NULL;
    
    unsigned long page_sz = (1 << PAGE_SHIFT);
    unsigned long first_pg_sz;
    
    void *user_num_entries = cmd->physlist;
    void *user_entries = ((void *)cmd->physlist) + offsetof(struct pinner_physlist, entries);
    
    //Allocate space for entries we'll copy to user space
    entries = kzalloc(p->num_pages * (sizeof(struct pinner_physlist_entry)), GFP_KERNEL);
    if (!entries) {
        printk(KERN_ALERT "pinner: could not allocate buffer of size [%lu]\n", p->num_pages * (sizeof(struct pinner_physlist_entry)));
        ret = -ENOMEM;
        goto send_physlist_cleanup;
    }
    
    
    //Now for the really ugly stuff: building up the physlist_entries with the 
    //right physical addresses and lengths
    
    //Special case for first page
    first_pg_sz = (unsigned) (page_sz - first_page_offset);
    entries[0].addr = page_to_phys(p->pages[0]) + first_page_offset;
    if (total_sz <= first_pg_sz) {
        //Only one thing to put in the physlist entries.
        entries[0].len = total_sz;
        goto done_building_entries; //TODO: write logic that doesn't use goto here
    }
    entries[0].len = first_pg_sz;
    total_sz -= first_pg_sz;
    
    //Middle pages
    for (i = 1; i < p->num_pages - 1; i++) {
        entries[i].addr = page_to_phys(p->pages[i]);
        entries[i].len = page_sz;
        total_sz -= page_sz;
    }
    
    //Special case for last page
    entries[i].addr = page_to_phys(p->pages[i]);
    entries[i].len = total_sz; //Maybe I shouldn't have reused the variable like this...
    
    done_building_entries:
    //Write the num_entries field of the user's pinner_physlist
    n = copy_to_user(user_num_entries, &(p->num_pages), sizeof(unsigned));
    if (n != 0) {
        printk(KERN_ALERT "pinner: could not copy num_entries to userspace\n");
        ret = -EAGAIN;
        goto send_physlist_cleanup;
    }
    //Write the entries themselves
    n = copy_to_user(user_entries, entries, p->num_pages * (sizeof(struct pinner_physlist_entry)));
    if (n  != 0) {
        printk(KERN_ALERT "pinner: could not copy entries to userspace\n");
        ret = -EAGAIN;
        goto send_physlist_cleanup;
    }
    
    send_physlist_cleanup:
    if (entries) kfree(entries);
    return ret;
}

static int pinner_do_pin(struct pinner_cmd *cmd, struct proc_info *info) {
    int ret = 0;
    
    struct pinning *pin = NULL;
    unsigned long start;
    unsigned long first_pg_offset;
    unsigned long page_sz = (1 << PAGE_SHIFT);
    unsigned long page_mask = (page_sz - 1);
    int num_pages;
    int n;
    struct page **p = NULL;
    
    struct pinner_handle usr_handle;
    
    //Validate inputs from user's command
    num_pages = (cmd->usr_buf_sz + page_mask) / page_sz; // = ceil(usr_buf_sz / page_sz)
    if (num_pages > PINNER_MAX_PAGES) {
        printk(KERN_ALERT "pinner: exceeded maximum pinning size\n");
        ret = -EINVAL;
        goto do_pin_error;
    } else if (num_pages <= 0) {
        printk(KERN_ALERT "pinner: invalid pinning size\n");
        ret = -EINVAL;
        goto do_pin_error;
    }
    
    //Attempt to pin pages 
    start = ((unsigned long)cmd->usr_buf | page_mask) - page_mask;
    first_pg_offset = (unsigned long)cmd->usr_buf - start;
    p = kmalloc(num_pages * (sizeof(struct page *)), GFP_KERNEL);
    if (!p) {
        printk(KERN_ALERT "pinner: could not allocate buffer of size [%lu]\n", num_pages * (sizeof(struct page *)));
        ret = -ENOMEM;
        goto do_pin_error;
    }
    n = get_user_pages_fast(start, num_pages, 1, p);
    if (n != num_pages) {
        //Could not pin all the pages. Just quit and ask the user to try again
        printk(KERN_ERR "pinner: could not satisfy user request\n");
        ret = -EAGAIN;
        goto do_pin_error;
    }
    
    //Maintain our own internal bookkeeping (i.e. add pinning info to list inside proc_info)
    pin = kzalloc(sizeof(struct pinning), GFP_KERNEL);
    if (!pin) {
        printk(KERN_ALERT "pinner: could not allocate buffer of size [%lu]\n", sizeof(struct pinning));
        ret = -ENOMEM;
        goto do_pin_error;
    }
    pin->num_pages = num_pages;
    pin->pages = p; //Note to self: look out for double-frees, since now this pointer is inside the pinning struct
    p = NULL; //For extra safety against double-freeing
    get_random_bytes(&(pin->magic), sizeof(pin->magic));
    list_add(&(pin->list), &(info->pinning_list)); //CAREFUL: list_add adds the first argument to the second
    
    //Write the physical address info back to userspace
    ret = pinner_send_physlist(cmd, pin, first_pg_offset, cmd->usr_buf_sz);
    if (ret < 0) {
        goto do_pin_error;
    }
    
    //Give the userspace program a handle that allows them to undo this pinning
    usr_handle.user_magic = info->magic;
    usr_handle.pin_magic = pin->magic;
    n = copy_to_user(cmd->handle, &usr_handle, sizeof(struct pinner_handle));
    if (n != 0) {
        printk(KERN_ALERT "pinner: could not copy handle to userspace\n");
        ret = -EAGAIN;
        goto do_pin_error;
    }
    
    return 0;
    
    do_pin_error:
    
    if (pin) {
        pinner_free_pinning(pin);
    } else if (p) {
        //This is in an else if, since p is inside the pinning struct and will
        //be freed in the call to pinner_free_pinning(p)
        put_page_list(p, num_pages);
        kfree(p);
    }
    return ret;
}

static int pinner_do_unpin(struct pinner_cmd *cmd, struct proc_info *info) {
    struct list_head *cur; //For iterating
    struct pinner_handle usr_handle;
    int n;
    struct pinning *found = NULL;
    
    //Copy handle from userspace
    n = copy_from_user(&usr_handle, cmd->handle, sizeof(struct pinner_handle));
    if (n != 0) {
        printk(KERN_ALERT "pinner: could not copy handle from userspace\n");
        return -EAGAIN;
    }
    
    //Ensure that the user's handle matches the correct user_magic. We want to
    //make it very difficult for buggy (or malicious) user code to accidentally
    //unpin someone else's pinnings
    if (usr_handle.user_magic != info->magic) {
        printk(KERN_ALERT "pinner: incorrect user handle. No unpinning was performed\n");
        return -EINVAL;
    }
    
    //Search linearly through pinning structs for correct pin_magic
    //Note: who cares if this is inefficient? It's not like this function is
    //getting called millions of times per second
    for (cur = info->pinning_list.next; cur != &(info->pinning_list); cur = cur->next) {
        struct pinning *p = list_entry(cur, struct pinning, list);
        if (usr_handle.pin_magic == p->magic) {
            found = p;
            break;
        }
    }
    
    if (!found) {
        printk(KERN_ALERT "pinner: incorrect pin handle. No unpinning was performed\n");
        return -EINVAL;
    }
    
    //Delete the pinning
    pinner_free_pinning(found);
    
    return 0;
}

static int pinner_open (struct inode *inode, struct file *filp) {
    struct proc_info *info = NULL;
    
	//Allocate and insert a new proc_info. Values should be initialized to zero
    info = kzalloc(sizeof(struct proc_info), GFP_KERNEL);
    if (!info) {
        printk(KERN_ALERT "Could not open pinner driver\n");
        return -ENOMEM;
    }
    
    //Initialize list of pinnings
    INIT_LIST_HEAD(&(info->pinning_list));
    
    //Initialize the magic
    get_random_bytes(&(info->magic), sizeof(info->magic));
    
    //Add to head of list
    mutex_lock(&users_mutex); //Need to watch out for race conditions
    list_add(&(info->list), &users);
    mutex_unlock(&users_mutex);
    
    //Keep link to this struct in filp->private_data
	filp->private_data = info;
    
    printk(KERN_ALERT "Succesfully opened pinner driver\n");
	return 0; //SUCCESS
}

static int pinner_release (struct inode *inode, struct file *filp) {
    struct proc_info *info = filp->private_data;
    
	//Clean up this proc_info struct
    pinner_free_proc_info(info);
    
    printk(KERN_ALERT "Closed pinner driver\n");
	return 0;
}

//Write function. Handles commands from userspace
static ssize_t pinner_write (struct file *filp, char const __user *buf, size_t sz, loff_t *off) {
    int rc;
    struct pinner_cmd cmd;
    struct proc_info *info = filp->private_data;
    
    if (sz != sizeof(struct pinner_cmd)) {
        printk(KERN_ALERT "pinner: bad command struct size [%lu], should be [%lu]\n", sz, sizeof(struct pinner_cmd));
        return -EINVAL;
    }
    
    rc = copy_from_user(&cmd, buf, sizeof(struct pinner_cmd));
    if (rc != 0) {
        printk(KERN_ALERT "pinner: could not copy command struct from userspace. Still need to copy [%d] bytes out of %lu\n", rc, sizeof(struct pinner_cmd));
        return -EAGAIN;
    }
    
    switch(cmd.cmd) {
        case PINNER_PIN:
            return pinner_do_pin(&cmd, info);
            break;
        case PINNER_UNPIN:
            return pinner_do_unpin(&cmd, info);
            break;
        case PINNER_FLUSH: {
            //Find the VMA containing the user's buffer
            struct vm_area_struct *vma = find_vma(current->mm, (unsigned long)cmd.usr_buf);
            if (!vma) {
                printk(KERN_ALERT "pinner: unrecognized user virtual address\n");
                return -EINVAL;
            }
            flush_cache_range(vma, (unsigned long) cmd.usr_buf, (unsigned long) cmd.usr_buf + cmd.usr_buf_sz);
            break;
        }
        default:
            printk(KERN_ALERT "pinner: unrecognized command code [%u]\n", cmd.cmd);
            return -ENOSYS;
    }
	
	return 0;
}


//Structs for registering with misc devices
static struct file_operations pinner_fops = {
	.open = pinner_open,
	.write = pinner_write,
	.release = pinner_release
};

static struct miscdevice pinner_miscdev = { 
	.minor = MISC_DYNAMIC_MINOR, 
	.name = "pinner",
	.fops = &pinner_fops,
	.mode = 0666
};

static int registered = 0;

static int __init pinner_init(void) { 
    int rc;
    
    //Now that everything is safely initialized, make the driver available:
	rc = misc_register(&pinner_miscdev);
	if (rc < 0) {
		printk(KERN_ALERT "Could not register pinner module\n");
	} else {
		printk(KERN_ALERT "pinner module inserted\n"); 
		registered = 1;
	}
    
	return rc; //Propagate error code
} 

static void pinner_exit(void) { 
    //Remove all pinnings and free all proc_infos	
	if (registered) misc_deregister(&pinner_miscdev);
	
    //Probably don't need to lock mutex, since driver has been unregistered
    //mutex_lock(&users_mutex);
    while (!list_empty(&users)) {
        printk(KERN_ALERT "Warning: pinner exit function is freeing things that should have already been freed...\n");
        pinner_free_proc_info(list_entry(users.next, struct proc_info, list));
    }
    //mutex_unlock(&users_mutex);
    
	printk(KERN_ALERT "pinner module removed\n"); 
} 

MODULE_LICENSE("Dual BSD/GPL"); 

module_init(pinner_init); 
module_exit(pinner_exit);
