// Copyright © 2019-2022 VMware, Inc. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0 OR MIT

//! An example that implements a replicated hashmap.
#![feature(generic_associated_types)]

use std::hint::black_box;
use std::num::NonZeroUsize;
use std::sync::Arc;
use std::sync::Barrier;
use std::thread;
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use env_logger;
use log::debug;
use nix::libc::REG_OLDMASK;
use node_replication::nr::ThreadToken;
use rand::seq::SliceRandom;
use rand::SeedableRng;

use node_replication::nr::NodeReplicated;

use bench_utils::{topology::*, pin_thread};

mod structs;
mod help_fn;

use structs::*;
use help_fn::*;

fn main() {
    // Setup logging and some constants.
    let _r = env_logger::try_init();

    // declare the thread function
    // let thread_loop = |
    //         replica: Arc<NodeReplicated<NrHashMap>>, core_id: u64, rid: usize, duration: Duration,
    //         start_sync: Arc<Barrier>, mut operations: Vec<Ops>, f: BenchFn| 
    //         -> (Core, u64, Vec<u128>) {
    //     pin_thread(core_id);

    //     let thread_token = replica
    //         .register(rid)
    //         .expect("Can't register replica, out of slots?");

    //     // let operations = generate_operations(NOP, write_ratio, KEY_SPACE, distribution);
    //     operations.shuffle(&mut rand::rngs::SmallRng::from_entropy());

    //     debug!(
    //         "Running {:?} on core {} replica#{} rtoken#{:?} for {:?}",
    //         thread::current().id(),
    //         core_id,
    //         rid,
    //         thread_token,
    //         duration
    //     );

    //     let mut latency: Vec<u128> = Vec::new();
    //     let mut operations_completed: u64 = 0;
    //     let mut iter: usize = 0;
    //     // let mut write_completed: u64 = 0;

    //     // prepopulate
    //     // if core_id == 0 {
    //     //     prepopulate(thread_token, &replica, KEY_SPACE, PREPOPULATE_RATIO);
    //     // }

    //     start_sync.wait();
    //     let start = Instant::now();
    //     let end_experiment = start + duration;
    //     while Instant::now() < end_experiment {
    //         let iter_start = Instant::now();
    //         for _i in 0..BATCH_SIZE {
    //             black_box((f)(
    //                 thread_token,
    //                 &replica,
    //                 &operations[iter],
    //             ));
    //             iter = (iter + 1) % NOP;
    //         }
    //         latency.push(iter_start.elapsed().as_nanos() as u128 / BATCH_SIZE as u128);
    //         operations_completed += 1 * BATCH_SIZE as u64;
    //     }

    //     debug!(
    //         "Completed {:?} on core {} replica#{} rtoken#{:?} did {} ops in {:?}",
    //         thread::current().id(),
    //         core_id,
    //         rid,
    //         thread_token,
    //         operations_completed,
    //         duration
    //     );

    //     start_sync.wait();
    //     // println!("{:?} completed write: {}", thread::current().id(), write_completed);
    //     (core_id, operations_completed, latency)
    // };
    
    let topology = MachineTopology::new();
    
    println!("generate_ops");
    let ops: Arc<Vec<Ops>> = generate_operations(NOP, WRITE_RATIO, KEY_SPACE, DIST).into();
    let rs = ReplicaStrategy::Socket;
    let tm = ThreadMapping::Interleave;
    let num_threads:Vec<usize> = [12, 14, 16, 18, 20].to_vec();
    // let ts = 20;
    let dur_u64: u64 = 20;
    let duration = Duration::from_secs(dur_u64);
    let func = |thread_token: ThreadToken, replica: &Arc<NodeReplicated<NrHashMap>>, op: &Ops| match op.opcode {
        OpCode::Get => {
            replica.execute(Access::Get(op.key), thread_token);
        }
        OpCode::CheckPut => {
            replica.execute_mut(Modify::CheckPut(op.key, op.val), thread_token);

            // match replica.execute(Access::Get(op.key), thread_token) {
            //     Some(_) => None,
            //     None => replica.execute_mut(Modify::Put(op.key, op.val), thread_token),
            // };
        }
        OpCode::Put => {
            // replica.execute(Access::Get(op.key), thread_token);
            replica.execute_mut(Modify::Put(op.key, op.val), thread_token);
        }
        OpCode::Delete => {
            // replica.execute(Access::Get(op.key), thread_token);
            replica.execute_mut(Modify::Delete(op.key), thread_token);
        }
    };

    for ts in num_threads.iter() {
        let start_sync = Arc::new(Barrier::new(*ts));

        let rm = replica_core_allocation(&topology, rs, tm, *ts);
        let num_replica = NonZeroUsize::new(rm.len()).unwrap();
        let nrht = Arc::new(NodeReplicated::<NrHashMap>::with_log_size(
            num_replica, linux_chg_affinity, LOG_SIZE).unwrap());

        println!("num_replica={}, rm={:?}", num_replica, rm);

        let mut handles: Vec<JoinHandle<(Core, u64, Vec<u128>)>> = Vec::new();

        for (rid, cores) in rm.clone().into_iter() {
            for core_id in cores {
                pin_thread(core_id);
                let start_sync = start_sync.clone();

                let nrht_cln = nrht.clone();
                let thread_duration =  duration.clone();
                let mut operations = ops.clone();
                let f = func.clone();

                let handle = thread::spawn(move || {
                    // thread_loop(
                    //     nrht_cln, core_id,
                    //     rid, thread_duration,
                    //     start_sync,
                    //     operations,
                    //     |thread_token, replica, op| match op.opcode {
                    //         OpCode::Get => {
                    //             replica.execute(Access::Get(op.key), thread_token);
                    //         }
                    //         OpCode::CheckPut => {
                    //             replica.execute_mut(Modify::CheckPut(op.key, op.val), thread_token);

                    //             // match replica.execute(Access::Get(op.key), thread_token) {
                    //             //     Some(_) => None,
                    //             //     None => replica.execute_mut(Modify::Put(op.key, op.val), thread_token),
                    //             // };
                    //         }
                    //         OpCode::Put => {
                    //             // replica.execute(Access::Get(op.key), thread_token);
                    //             replica.execute_mut(Modify::Put(op.key, op.val), thread_token);
                    //         }
                    //         OpCode::Delete => {
                    //             // replica.execute(Access::Get(op.key), thread_token);
                    //             replica.execute_mut(Modify::Delete(op.key), thread_token);
                    //         }
                    //     }
                    // )
                    pin_thread(core_id);

                    let thread_token = nrht_cln
                        .register(rid)
                        .expect("Can't register replica, out of slots?");

                    // let operations = generate_operations(NOP, write_ratio, KEY_SPACE, distribution);
                    let mut operations = (*operations).clone();
                    operations.shuffle(&mut rand::rngs::SmallRng::from_entropy());

                    debug!(
                        "Running {:?} on core {} replica#{} rtoken#{:?} for {:?}",
                        thread::current().id(),
                        core_id,
                        rid,
                        thread_token,
                        duration
                    );

                    let mut latency: Vec<u128> = Vec::new();
                    let mut operations_completed: u64 = 0;
                    let mut iter: usize = 0;
                    let nop = NOP;
                    let batch_size = BATCH_SIZE;
                    // let mut write_completed: u64 = 0;

                    // prepopulate
                    // if core_id == 0 {
                    //     prepopulate(thread_token, &replica, KEY_SPACE, PREPOPULATE_RATIO);
                    // }

                    start_sync.wait();
                    let start = Instant::now();
                    let end_experiment = start + duration;
                    while Instant::now() < end_experiment {
                        let iter_start = Instant::now();
                        for _i in 0..batch_size {
                            black_box((f)(
                                thread_token,
                                &nrht_cln,
                                &operations[iter],
                            ));
                            iter = (iter + 1) % nop;
                        }
                        latency.push(iter_start.elapsed().as_nanos() as u128 / batch_size as u128);
                        operations_completed += 1 * batch_size as u64;
                    }

                    debug!(
                        "Completed {:?} on core {} replica#{} rtoken#{:?} did {} ops in {:?}",
                        thread::current().id(),
                        core_id,
                        rid,
                        thread_token,
                        operations_completed,
                        duration
                    );

                    start_sync.wait();
                    // println!("{:?} completed write: {}", thread::current().id(), write_completed);
                    (core_id, operations_completed, latency)
                });
                handles.push(handle);
            }
        }

        let mut everything = Vec::<u128>::new();
        let mut ops_count = Vec::<u64>::new();

        for (tid, handle) in handles.into_iter().enumerate() {
            let (cid, num_ops, thread_results) = handle.join().unwrap();
            everything.extend(&thread_results);
            ops_count.push(num_ops);
        }

        let avg = mean_128(&everything).unwrap();
        let avg_ops = ops_count.iter().sum::<u64>() as f64;
        println!(
            "Run({:?} {:?} {:?} {:.2} op/s) => {:.5} ns/op",
            rs,
            tm,
            ts,
            avg_ops / dur_u64 as f64,
            avg,
        );
        // println!(
        //     "everything size = {}, ops_count size = {}",
        //     everything.len(), ops_count.len(),
        // );
        println!();
    }
}
