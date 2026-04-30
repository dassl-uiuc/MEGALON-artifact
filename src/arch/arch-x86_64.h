#ifndef FIO_ARCH_X86_H
#define FIO_ARCH_X86_H

#define cpu_relax() __asm__ __volatile__("rep;nop" : : : "memory")
#define read_barrier() __asm__ __volatile__("" : : : "memory")
#define write_barrier() __asm__ __volatile__("" : : : "memory")

#endif /* FIO_ARCH_X86_H */
