// Copyright © 2019-2022 VMware, Inc. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0 OR MIT

//! An example that implements a replicated hashmap.
#![feature(generic_associated_types)]

use std::collections::HashMap;
use std::num::NonZeroUsize;
use std::sync::Arc;

use nix::libc;
use nix::sched::{self, CpuSet};
use nix::unistd::Pid;

use node_replication::nr::Dispatch;
use node_replication::nr::NodeReplicated;
use node_replication::nr::AffinityChange;

use bench_utils::topology::MACHINE_TOPOLOGY;

// struct MachineTopology;

// impl MachineTopology {
//     pub fn cpus_on_node(node: u64) -> Vec<u32> {
//         // Implement your logic here
//         vec![0, 1] // Example CPU IDs
//     }
// }

#[derive(Clone, Debug, Eq, Hash, PartialEq)]
struct GcdKey {
    field1: u64,
    field2: u64,
    field3: u64,
}

/// The node-replicated hashmap uses a std hashmap internally.
#[derive(Default, Clone)]
struct NrHashMap {
    storage: HashMap<GcdKey, u64>,
}

/// We support a mutable put operation to insert a value for a given key.
#[derive(Clone, Debug, PartialEq)]
enum Modify {
    /// Insert (key, value)
    Put(GcdKey, u64),
}

/// We support an immutable read operation to lookup a key from the hashmap.
#[derive(Clone, Debug, PartialEq)]
enum Access {
    // Retrieve key.
    Get(GcdKey),
}

/// The Dispatch trait executes `ReadOperation` (our Access enum) and `WriteOperation`
/// (our `Modify` enum) against the replicated data-structure.
impl Dispatch for NrHashMap {
    type ReadOperation<'rop> = Access;
    type WriteOperation = Modify;
    type Response = Option<u64>;

    /// The `dispatch` function contains the logic for the immutable operations.
    fn dispatch<'rop>(&self, op: Self::ReadOperation<'rop>) -> Self::Response {
        match op {
            Access::Get(key) => self.storage.get(&key).map(|v| *v),
        }
    }

    /// The `dispatch_mut` function contains the logic for the mutable operations.
    fn dispatch_mut(&mut self, op: Self::WriteOperation) -> Self::Response {
        match op {
            Modify::Put(key, value) => self.storage.insert(key, value),
        }
    }
}

fn linux_chg_affinity(af: AffinityChange) -> usize {
    match af {
        // System requests to change affinity to replica with `rid`
        AffinityChange::Replica(rid) => {
            // figure out where we're currently running:
            let mut cpu: usize = 0;
            let mut node: usize = 0;
            unsafe { nix::libc::syscall(nix::libc::SYS_getcpu, &mut cpu, &mut node, 0) };

            // figure out all cores we can potentially on run for
            // correct affinity with new rid:
            let mut cpu_set = nix::sched::CpuSet::new();
            for ncpu in MACHINE_TOPOLOGY.cpus_on_node(rid as u64) {
                cpu_set.set(ncpu.cpu as usize);
            }
            // pin current thread to these cores
            nix::sched::sched_setaffinity(nix::unistd::Pid::from_raw(0), &cpu_set);
            // return the cpu id where we were originally running on
            cpu as usize
        }
        // System requests to revert affinity to original replica (`old`)
        AffinityChange::Revert(core_id) => {
            // `core_id` is the cpu number we returned above
            let mut cpu_set = nix::sched::CpuSet::new();
            cpu_set.set(core_id);
            // Migrate thread back to old core_id
            nix::sched::sched_setaffinity(nix::unistd::Pid::from_raw(0), &cpu_set);
            0x0 // return value is ignored for `Revert`
        }
    }
}

/// We initialize one log and `x` replicas for a hashmap. We register with the replica and
/// then execute operations on `y` threads.
fn main() {
    // Setup logging and some constants.
    let _r = env_logger::try_init();
    const PER_THREAD_OPS: u64 = 2000000; // How many ops each thread performs

    // Next, we create the NodeReplicated HashMap (we run this code 4 times with different
    // amounts of replicas for demonstration purposes)
    for replica_cntr in 1..=2 {
        let num_replica = NonZeroUsize::new(replica_cntr).unwrap();
        let nrht = Arc::new(NodeReplicated::<NrHashMap>::new(num_replica, linux_chg_affinity).unwrap());

        // The replica executes a Modify or Access operations by calling
        // `execute_mut` and `execute`. Eventually they end up in the `Dispatch` trait.
        let thread_loop = |replica: Arc<NodeReplicated<NrHashMap>>, ttkn| {
            let now = std::time::Instant::now();
            for i in 0..PER_THREAD_OPS {
                let _r = match i % 2 {
                    0 => {
                        let key: GcdKey = GcdKey{
                            field1: i,
                            field2: i,
                            field3: i,
                        };
                        replica.execute_mut(Modify::Put(key, i + 1), ttkn)
                    }
                    1 => {
                        let key: GcdKey = GcdKey{
                            field1: i,
                            field2: i,
                            field3: i,
                        };
                        replica.execute_mut(Modify::Put(key, i + 1), ttkn)
                    }
                    // 1 => {
                    //     let key: GcdKey = GcdKey{
                    //         field1: i - 1,
                    //         field2: i - 1,
                    //         field3: i - 1,
                    //     };
                    //     let response: Option<u64> = replica.execute(Access::Get(key), ttkn);
                    //     assert_eq!(response, Some(i));
                    //     response
                    // }
                    _ => unreachable!(),
                };
            }
            let elapsed = now.elapsed();
            println!(
                "({} ns/op)",
                elapsed.as_nanos() / PER_THREAD_OPS as u128
            );
        };

        // We spawn a number of threads that we distribute on the replicas the threads
        // will then issue operations against the replicas. We again run this 4 times
        // with different thread counts for demonstration purposes.
        for thread_num in 1..=10 {
            let ttl_thread_num = thread_num * replica_cntr;
            println!(
                "Running with {} replicas and {} threads",
                replica_cntr, ttl_thread_num
            );

            let mut threads = Vec::with_capacity(ttl_thread_num);
            for t in 0..ttl_thread_num {
                let nrht_cln = nrht.clone();
                threads.push(std::thread::spawn(move || {
                    let ttkn = nrht_cln.register(t % replica_cntr).expect(
                        format!(
                            "Unable to register thread with replica {}",
                            t % replica_cntr
                        )
                        .as_str(),
                    );
                    thread_loop(nrht_cln, ttkn);
                }));
            }

            // Wait for all the threads to finish
            for thread in threads {
                thread.join().unwrap();
            }

            println!();
        }
    }
}
