#ifndef FIO_ARCH_GENERIC_H
#define FIO_ARCH_GENERIC_H

#define cpu_relax() \
    do {            \
    } while (0)
#define read_barrier() __asm__ __volatile__("" : : : "memory")
#define write_barrier() __asm__ __volatile__("" : : : "memory")

#endif /* FIO_ARCH_GENERIC_H */
