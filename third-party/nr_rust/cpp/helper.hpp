#include <numa.h>
#include <numaif.h>

void pin_thread_to_numa_node(int numa_node) {
    if (numa_node < 0 || numa_node > numa_max_node()) {
        std::cerr << "invalid numa node" << std::endl;
        return;
    }

    struct bitmask* cpumask = numa_allocate_cpumask();
    numa_node_to_cpus(numa_node, cpumask);

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);

    for (unsigned int i = 0; i < cpumask->size; ++i) {
        if (numa_bitmask_isbitset(cpumask, i)) {
            CPU_SET(i, &cpuset);
        }
    }
    numa_free_cpumask(cpumask);
    pthread_t thread = pthread_self();
    if (pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset) != 0) {
        std::cerr << "pthread_setaffinity_np failed" << std::endl;
        return;
    }

    std::cout << "thread pinned to NUMA node " << numa_node << std::endl;
}