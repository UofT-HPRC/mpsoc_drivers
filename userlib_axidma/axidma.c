#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include "axidma.h"
#include "pinner.h"

//This cleans up the code slightly. I didn't use a typedef because I was worried
//about conflicts once this becomes a shared library.
#define handle  struct pinner_handle
#define physlist struct pinner_physlist

#define AXI_DMA_REG_SPAN 0x1000

#define DBG_PRINT
//#define DBG_PRINT(format, val) fprintf(stderr, #val " = " format "\n", val)

//Format of the AXI DMA's registers
typedef struct {
    uint32_t    MM2S_DMACR;
    uint32_t    MM2S_DMASR;
    uint32_t    MM2S_curdesc_lsb;
    uint32_t    MM2S_curdesc_msb;
    uint32_t    MM2S_taildesc_lsb;
    uint32_t    MM2S_taildesc_msb;
    
    uint32_t    unused[6];
    
    uint32_t    S2MM_DMACR;
    uint32_t    S2MM_DMASR;
    uint32_t    S2MM_curdesc_lsb;
    uint32_t    S2MM_curdesc_msb;
    uint32_t    S2MM_taildesc_lsb;
    uint32_t    S2MM_taildesc_msb;
} axidma_regs;

//Functions to open and close an AXI DMA context.
axidma_ctx* axidma_open(char const* path) {
    int fd = -1;
    void *reg_base = MAP_FAILED;
    
    fd = open(path, O_RDWR);
    if (fd == -1) {
        perror("Could not open AXI DMA UIO file");
        goto axidma_open_error;
    }
    
    reg_base = mmap(0, AXI_DMA_REG_SPAN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (reg_base == MAP_FAILED) {
        perror("Could not mmap AXI DMA registers");
        goto axidma_open_error;
    }
    
    axidma_ctx *ret = malloc(sizeof(axidma_ctx));
    if (!ret) {
        perror("Could not allocate axidma_ctx struct");
        goto axidma_open_error;
    }
    
    ret->fd = fd;
    ret->reg_base = reg_base;
    return ret;
    
    axidma_open_error:
    if (fd != -1) close(fd);
    if (reg_base != MAP_FAILED) munmap(reg_base, AXI_DMA_REG_SPAN);
    return NULL;
}


void axidma_close(axidma_ctx *ctx) {
    close(ctx->fd);
    munmap(ctx->reg_base, AXI_DMA_REG_SPAN);
    free(ctx);
}

//Some helper functions for the linked list

static inline void sg_entry_init(sg_entry *node) {
    node->next = node;
    node->prev = node;
}

static void sg_entry_add_before(sg_entry *head, sg_entry *new) {
    
    sg_entry *oldtail = head->prev;
    
    oldtail->next = new;
    new->prev = oldtail;
    
    new->next = head;
    head->prev = new;
}

//This is not a general-purpose function. It adds the list pointed at by
//sentinel, but not sentinel itself.
static void sg_entry_add_list_before(sg_entry *head, sg_entry *sentinel) {
    sg_entry *oldtail = head->prev;
    
    oldtail->next = sentinel->next;
    sentinel->next->prev = oldtail;
    
    head->prev = sentinel->prev;
    sentinel->prev->next = head;
}

//Create an sg_list object
sg_list *axidma_list_new(void *sg_buf, physlist const *sg_plist,
                         void const *data_buf, physlist const *data_plist) 
{
    sg_list *lst = malloc(sizeof(sg_list));
    if (!lst) {
        perror("Could not allocate sg_list");
        return NULL;
    }
    
    sg_entry_init(&(lst->sentinel));
    
    lst->sg_buf = sg_buf;
    lst->sg_plist = sg_plist;
    lst->sg_offset = 0;
    
    lst->data_buf = data_buf;
    lst->data_plist = data_plist;
    lst->data_offset = 0;
    
    return lst;
}


/*
 * Clears all the entries in an sg_list, except the sentinel 
*/
void axidma_free_list(sg_entry *sentinel) {    
    //Step through linked list and free all the entries
    sg_entry *e = sentinel->next;
    do {
        sg_entry *tmp = e->next;
        free(e);
        e = tmp;
    } while (e != sentinel);
}

//Free an sg_list object
void axidma_list_del(sg_list *lst) {
    //Gracefully do nothing if lst is NULL
    if (!lst) return;
    axidma_free_list(&(lst->sentinel));
    free(lst);
}

//Helper functions for dealing with physlists

//Find the index of the entry which contains the byte at offset past the start
//of the buffer (in virtual memory). Sets offset_in_entry to be the offset into
//this particular entry. Returns -1 if not found.
int get_entry_index(physlist const *plist, unsigned offset, unsigned *offset_in_entry) {
    //Doesn't have to be efficient. Just does a dumb linear search
    int i;
    for (i = 0; i < plist->num_entries; i++) {
        if (offset >= plist->entries[i].len) {
            offset -= plist->entries[i].len;
            continue;
        } 
        *offset_in_entry = offset;
        return i; //Found
    }
    
    return -1; //Not found
}

//Convert offset into virtual buffer into physical address. Returns NULL if not
//found.
uint64_t virt_to_phys(physlist const *plist, unsigned offset) {
    //Doesn't have to be efficient. Just does a dumb linear search
    for (int i = 0; i < plist->num_entries; i++) {
        if (offset >= plist->entries[i].len) {
            offset -= plist->entries[i].len;
            continue;
        }
        return plist->entries[i].addr + offset;
    }
    //Error: not found in physlist
    return (uint64_t) NULL;
}

//Scatter-gather entries must be in contiguous memory. This function walks 
//through a physlist to find the next chunk of size sz after offset, and returns
//the offset. Returns AXIDMA_NOT_FOUND if nothing could be found
static unsigned find_contiguous_after(physlist const *plist, unsigned offset, unsigned sz) {
    unsigned offset_in_entry;
    int ind = get_entry_index(plist, offset, &offset_in_entry);
    if (ind == -1) {
        //No need to print anything, another function will deal with the error
        return AXIDMA_NOT_FOUND;
    }
    
    //Check if the desired sz can fit at the found location
    unsigned space_left = plist->entries[ind].len - offset_in_entry;
    if (sz <= space_left) {
        return offset; //Original offset will work
    }
    
    //Desired size won't fit in remaining space of the entry. Try walking 
    //through to find another spot
    offset += space_left;
    ind++;
    while (ind < plist->num_entries) {
        if (sz <= plist->entries[ind].len) {
            //We can fit here
            return offset;
        } 
        offset += plist->entries[ind].len;
        ind++;
    }
    
    //If we got here, it means no space was found
    return AXIDMA_NOT_FOUND;
    
}

/*
 * Apportions a new buffer from the the user's data buffer, and appends the
 * necessary entries to the SG list
 * 
 * Returns 0 on success, SG_OUT_OF_MEM if there is no space for the next SG 
 * entry, or BUF_OUT_OF_MEM if there is no space for the desired buffer
*/
int axidma_add_entry(sg_list *lst, unsigned sz) {
    //Before we go down this road, check that the function arugments make sense
    if (!lst || !sz) {
        fprintf(stderr, "axidma_add_entry: Invalid function argument\n");
        return AXIDMA_OTHER_ERROR;
    }
    
    //Set up some variables we'll be using. We do not modify the state of lst
    //until we're sure that everything would succeed. These next few variables
    //just hold on to the prospective state changes
    int ret = 0;
    
    //Will hold a temporary list of SG entries
    sg_entry sentinel; 
    sg_entry_init(&sentinel);
    
    //These will be the new values of data_offset and sg_offset in lst
    unsigned data_offset = lst->data_offset;
    unsigned sg_offset = lst->sg_offset;
    
    
    //First, check if there is space in the buffer memory. While we do that, 
    //we'll keep track of the scatter-gather entries we need to build
    unsigned offset_in_entry;
    int ind = get_entry_index(lst->data_plist, data_offset, &offset_in_entry);
    if (ind == -1) {
        ret = BUF_OUT_OF_MEM;
        goto axidma_add_entry_error;
    }
    
    //Now we begin the complicated process of building up the SG entries
    //This does NOT modify lst; we wait until everything would succeed before
    //doing that
    while (sz != 0) {
        //Space left in this entry of the data physlist
        unsigned space = lst->data_plist->entries[ind].len - offset_in_entry;
        
        //Check if there would be room for an SG descriptor
        sg_offset = find_contiguous_after(lst->sg_plist, sg_offset, sizeof(sg_descriptor));
        if (sg_offset == AXIDMA_NOT_FOUND) {
            ret = SG_OUT_OF_MEM;
            goto axidma_add_entry_error;
        }
        
        //Make an SG descriptor
        sg_entry *e = malloc(sizeof(sg_entry));
        if (!e) {
            perror("Could not allocate sg_entry");
            ret = AXIDMA_OTHER_ERROR;
            goto axidma_add_entry_error;
        }
        e->sg_offset = sg_offset;
        e->buf_phys = virt_to_phys(lst->data_plist, data_offset);
        sg_entry_add_before(&sentinel, e);
        e->is_EOF = 0; //These get set later
        e->is_SOF = 0; //ditto
        
        //Perform increments and check termination conditions
        offset_in_entry = 0;
        sg_offset += sizeof(sg_descriptor);
        if (space < sz) {
            //We'll need to do another iteration after this
            sz -= space;
            e->len = space;
            data_offset += space;
        } else {
            //We're done!
            e->len = sz;
            data_offset += sz;
            break;
        }
        //Try next entry in data_physlist, if there is one
        ind++;
        if (ind >= lst->data_plist->num_entries) {
            //No entries left
            ret = BUF_OUT_OF_MEM;
            goto axidma_add_entry_error;
        }
    }
    
    //Set SOF and EOF:
    sentinel.next->is_SOF = 1;
    sentinel.prev->is_EOF = 1;
    
    //At this point, we know we're finally safe to modify lst
    
    sg_entry_add_list_before(&(lst->sentinel), &sentinel);
    lst->sg_offset = sg_offset;
    lst->data_offset = data_offset;
    
    return 0; //Success
    
    axidma_add_entry_error:
    
    axidma_free_list(&sentinel);
    return ret;
}

//Actually writes an entry into RAM
static void s2mm_write_sg_entry(void *sg_buf, physlist const *sg_plist, sg_entry *e) {
    DBG_PRINT("%d", e->sg_offset);
    DBG_PRINT("%d", e->len);
    DBG_PRINT("%d", e->is_SOF);
    DBG_PRINT("%d", e->is_EOF);
    DBG_PRINT("%lx", e->buf_phys);
    DBG_PRINT("%c", '\n');
    
    volatile sg_descriptor *desc = (volatile sg_descriptor *) (sg_buf + e->sg_offset);
    //Is endianness gonna bite me for this?
    desc->control.sof = e->is_SOF;
    desc->control.eof = e->is_EOF;
    desc->control.len = e->len;
    desc->buffer_lsb = (uint32_t) (e->buf_phys & 0xFFFFFFFF);
    desc->buffer_msb = (uint32_t) ((e->buf_phys>>32) & 0xFFFFFFFF);
    
    if (!e->is_EOF) {
        uint64_t nextdesc_phys = virt_to_phys(sg_plist, e->next->sg_offset);
        desc->next_desc_lsb = (uint32_t) (nextdesc_phys & 0xFFFFFFFF);
        desc->next_desc_msb = (uint32_t) ((nextdesc_phys>>32) & 0xFFFFFFFF);
    }
}

/*
 * Writes the scatter-gather list entries to memory, then starts the transfer.
 * TODO: find clean way to return information about transfer status
*/
void axidma_s2mm_transfer(axidma_ctx *ctx, sg_list *lst) {
    //Validate inputs, just in case
    if (!ctx) {
        fprintf(stderr, "axidma_s2mm_transfer: invalid NULL context\n");
        return;
    }
    if (!lst) {
        fprintf(stderr, "axidma_s2mm_transfer: invalid NULL list\n");
        return;
    }
    if (lst->sentinel.next == &(lst->sentinel)) {
        fprintf(stderr, "axidma_s2mm_transfer: invalid list with no SG entries\n");
        return;
    }
    
    //Step through linked list of SG entries and write each one to RAM
    for (sg_entry *e = lst->sentinel.next; e != &(lst->sentinel); e = e->next) {
        s2mm_write_sg_entry(lst->sg_buf, lst->sg_plist, e);
    }
    
    //Now we actually send the commands to the AXI DMA's registers
    //This follows the programming sequence in the product guide. First, we 
    //write the pointer to the first descriptor
    volatile axidma_regs *regs = (volatile axidma_regs *) ctx->reg_base;
    
    uint64_t curdesc_phys = virt_to_phys(lst->sg_plist, lst->sentinel.next->sg_offset);
    regs->S2MM_curdesc_lsb = (uint32_t) (curdesc_phys & 0xFFFFFFFF);
    regs->S2MM_curdesc_msb = (uint32_t) ((curdesc_phys>>32) & 0xFFFFFFFF);
    
    //Enable IOC interrupts, and set run/stop to 1
    regs->S2MM_DMACR = 0b1000000000001; 
    
    //Now write the pointer to the last descriptor. This starts the transfer
    uint64_t taildesc_phys = virt_to_phys(lst->sg_plist, lst->sentinel.prev->sg_offset);
    regs->S2MM_taildesc_lsb = (uint32_t) (taildesc_phys & 0xFFFFFFFF);
    regs->S2MM_taildesc_msb = (uint32_t) ((taildesc_phys>>32) & 0xFFFFFFFF);
    
    
    //At this point, transfer has started. Wait for the interrupt!
    puts("Waiting for DMA to finish!");
    fflush(stdout);
    
    unsigned pending;
    read(ctx->fd, &pending, sizeof(pending));
    
    return;
}



#undef physlist
#undef handle
