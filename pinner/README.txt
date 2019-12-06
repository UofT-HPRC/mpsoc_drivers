Memory-pinning driver for Linux.

Goal is to allow a userspace program to ask the kernel to pin some of their 
pages in RAM, and that the kernel will pass back the physical addresses. This 
will form part of a larger strategy to make it easier to use the AXI DMA.
