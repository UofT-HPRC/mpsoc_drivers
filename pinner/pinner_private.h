#ifndef PINNER_PRIVATE_H
#define PINNER_PRIVATE_H 1

struct pinning {
    struct list_head list;
    unsigned num_pages;
    struct page **pages;
    unsigned magic; //Helps prevent problems where the user accidentally (or
    //on purpose) fiddled around with the handle we gave them. Should be generated
    //with get_random_bytes.
};

struct proc_info {
    struct list_head list;
    struct list_head pinning_list;
    unsigned magic; //Helps prevent problems where the user accidentally (or
    //on purpose) fiddled around with the handle we gave them. Should be generated
    //with get_random_bytes.
};

#endif
