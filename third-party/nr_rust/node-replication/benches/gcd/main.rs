//! Defines a hash-map that can be replicated.
#![allow(dead_code)]
#![feature(generic_associated_types)]

use std::collections::HashMap;
use std::fmt::Debug;
use std::fs::File;
use std::marker::Sync;
use std::path::Path;
use std::io::{self, BufRead};

use bench_utils::cnr_mkbench::ReplicaStrategy;
use logging::{debug, warn};
use rand::seq::SliceRandom;
use rand::{distributions::Distribution, Rng, RngCore};
use zipf::ZipfDistribution;

use bench_utils::benchmark::*;
use bench_utils::mkbench::{self, DsInterface};
use bench_utils::topology::ThreadMapping;
use bench_utils::Operation;
use node_replication::nr::{Dispatch, NodeReplicated};

// mod hashmap_comparisons;

// use hashmap_comparisons::*;

/// The initial amount of entries all Hashmaps are initialized with
#[cfg(feature = "smokebench")]
pub const INITIAL_CAPACITY: usize = 1 << 22; // ~ 4M
#[cfg(not(feature = "smokebench"))]
// pub const INITIAL_CAPACITY: usize = 1 << 26; // ~ 67M
pub const INITIAL_CAPACITY: usize = 10_000_000; // ~ 67M

// Biggest key in the hash-map
#[cfg(feature = "smokebench")]
pub const KEY_SPACE: usize = 5_000_000;
#[cfg(not(feature = "smokebench"))]
pub const KEY_SPACE: usize = 50_000_000;

// Key distribution for all hash-maps [uniform|skewed]
pub const UNIFORM: &'static str = "uniform";
pub const SKEWED: &'static str = "skewed";
// pub const OPERATION_PREFIX: &'static str = "/proj/rasl-PG0/sgbhat3/ycsb-traces/zipfian/runc/run.c.";
pub const OPERATION_PREFIX: &'static str = "/proj/rasl-PG0/sgbhat3/ycsb-traces/zipfian/run90/run.90.";
// pub const OPERATION_PREFIX: &'static str = "";
pub const LOAD_PREFIX: &'static str = "/proj/rasl-PG0/sgbhat3/ycsb-traces/load/run.load.";
pub const NUMA: bool = true;

// Number of operation for test-harness.
#[cfg(feature = "smokebench")]
pub const NOP: usize = 2_500_000;
#[cfg(not(feature = "smokebench"))]
pub const NOP: usize = 25_000_000;

#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub struct GcdKey {
    field1: u64,
    field2: u64,
    field3: u64,
}

/// Operations we can perform on the stack.
#[derive(Debug, Eq, PartialEq, Clone, Copy)]
pub enum OpWr {
    /// Add an item to the hash-map.
    Put(GcdKey, u64),
    CheckPut(GcdKey, u64),
}

#[derive(Debug, Eq, PartialEq, Clone, Copy)]
pub enum OpRd {
    /// Get item from the hash-map.
    Get(GcdKey),
}

/// When using a concurrent data-structure (as a comparison)
/// all operations go as read-only ops
#[derive(Debug, Eq, PartialEq, Clone, Copy)]
pub enum OpConcurrent {
    /// Get item from the hash-map.
    Get(GcdKey),
    /// Add an item to the hash-map.
    Put(GcdKey, u64),
}

/// Single-threaded implementation of the stack
///
/// We just use a vector.
#[derive(Debug, Clone)]
pub struct NrHashMap {
    storage: HashMap<GcdKey, u64>,
}

impl NrHashMap {
    pub fn put(&mut self, key: GcdKey, val: u64) {
        self.storage.insert(key, val);
    }

    pub fn checkput(&mut self, key: GcdKey, val: u64) {
        if !self.storage.contains_key(&key) {
            self.storage.insert(key, val);
        }
    }

    pub fn get(&self, key: GcdKey) -> Option<u64> {
        self.storage.get(&key).map(|v| *v)
    }
}

impl Default for NrHashMap {
    /// Return a dummy hash-map with `INITIAL_CAPACITY` elements.
    fn default() -> NrHashMap {
        let mut storage = HashMap::with_capacity(INITIAL_CAPACITY * 3);
        if OPERATION_PREFIX.is_empty() {
            for i in 0..INITIAL_CAPACITY {
                let key: GcdKey = GcdKey{
                    field1: i as u64,
                    field2: i as u64 ,
                    field3: i as u64 ,
                };
                storage.insert(key, (i + 1) as u64);
            }
        }
        NrHashMap { storage }
    }
}

impl Dispatch for NrHashMap {
    type ReadOperation<'rop> = OpRd;
    type WriteOperation = OpWr;
    type Response = Result<Option<u64>, ()>;

    fn dispatch<'rop>(&self, op: Self::ReadOperation<'rop>) -> Self::Response {
        match op {
            OpRd::Get(key) => return Ok(self.get(key))
        }
    }

    /// Implements how we execute operation from the log against our local stack
    fn dispatch_mut(&mut self, op: Self::WriteOperation) -> Self::Response {
        match op {
            OpWr::Put(key, val) => {
                self.put(key, val);
                Ok(None)
            },
            OpWr::CheckPut(key, val) => {
                self.checkput(key, val);
                Ok(None)
            }
        }
    }
}

/// Generate a random sequence of operations
///
/// # Arguments
///  - `nop`: Number of operations to generate
///  - `write`: true will Put, false will generate Get sequences
///  - `span`: Maximum key
///  - `distribution`: Supported distribution 'uniform' or 'skewed'
pub fn generate_operations(
    nop: usize,
    write_ratio: usize,
    span: usize,
    distribution: &'static str,
) -> Vec<Operation<OpRd, OpWr>> {
    assert!(distribution == "skewed" || distribution == "uniform");

    let mut ops = Vec::with_capacity(nop);

    let skewed = distribution == "skewed";
    let mut t_rng = rand::thread_rng();
    let zipf = ZipfDistribution::new(span, 0.99).unwrap();

    for idx in 0..nop {
        let id = if skewed {
            zipf.sample(&mut t_rng) as u64
        } else {
            // uniform
            t_rng.gen_range(0..span as u64)
        };

        let key: GcdKey = GcdKey{
            field1: id,
            field2: id,
            field3: id,
        };
        if idx % 100 < write_ratio {
            ops.push(Operation::WriteOperation(OpWr::CheckPut(key, t_rng.next_u64())));
        } else {
            ops.push(Operation::ReadOperation(OpRd::Get(key)));
        }
    }

    ops.shuffle(&mut t_rng);
    ops
}

fn string_to_u64(input: String) -> Result<u64, std::num::ParseIntError> {
    input.parse::<u64>()
}

pub fn string_to_GcdKey(input: String) -> Result<GcdKey, std::num::ParseIntError> {
    match string_to_u64(input) {
        Ok(num) => Ok(GcdKey {
            field1: num,
            field2: num,
            field3: num,
        }),
        Err(e) => Err(e),
    }
}

pub fn load_operations(
    file_name: &str,
) -> io::Result<Vec<Operation<OpRd, OpWr>>> {
    println!("load from {}", file_name);
    let mut ops = Vec::new();
    let path = Path::new(file_name);
    let file = File::open(&path)?;
    let reader = io::BufReader::new(file);

    let mut t_rng = rand::thread_rng();

    for line in reader.lines() {
        let line = line?;
        let parts: Vec<&str> = line.split_whitespace().collect();

        if parts.len() < 2 {
            continue; // Skip malformed lines
        }
        
        let input: String = parts[1].to_string().replace("user", "");
        if parts[0] == "R" {
            match string_to_GcdKey(input) {
                Ok(gcd_key) => {
                    ops.push(Operation::ReadOperation(OpRd::Get(gcd_key)));
                }
                Err(_e) => {
                    debug!("failed to parse GcdKey");
                }
            }
        } else if parts[0] == "I" {
            match string_to_GcdKey(input) {
                Ok(gcd_key) => {
                    ops.push(Operation::WriteOperation(OpWr::CheckPut(gcd_key, t_rng.next_u64())));
                }
                Err(_e) => {
                    debug!("failed to parse GcdKey");
                }
            }
        }
    }

    println!("num loaded {}", ops.len());
    Ok(ops)
}

/// Generate a random sequence of operations
///
/// # Arguments
///  - `nop`: Number of operations to generate
///  - `write`: true will Put, false will generate Get sequences
///  - `span`: Maximum key
///  - `distribution`: Supported distribution 'uniform' or 'skewed'
pub fn generate_operations_concurrent(
    nop: usize,
    write_ratio: usize,
    span: usize,
    distribution: &'static str,
) -> Vec<Operation<OpConcurrent, ()>> {
    assert!(distribution == "skewed" || distribution == "uniform");

    let mut ops = Vec::with_capacity(nop);

    let skewed = distribution == "skewed";
    let mut t_rng = rand::thread_rng();
    let zipf = ZipfDistribution::new(span, 1.03).unwrap();

    for idx in 0..nop {
        let id = if skewed {
            zipf.sample(&mut t_rng) as u64
        } else {
            // uniform
            t_rng.gen_range(0..span as u64)
        };

        let key: GcdKey = GcdKey{
            field1: id,
            field2: id,
            field3: id,
        };
        if idx % 100 < write_ratio {
            ops.push(Operation::ReadOperation(OpConcurrent::Put(
                key,
                t_rng.next_u64(),
            )));
        } else {
            ops.push(Operation::ReadOperation(OpConcurrent::Get(key)));
        }
    }

    ops.shuffle(&mut t_rng);
    ops
}

/// Compare a replicated hashmap against a single-threaded implementation.
fn hashmap_single_threaded(c: &mut TestHarness) {
    // Size of the log.
    const LOG_SIZE_BYTES: usize = 2 * 1024 * 1024;
    // How many operations per iteration
    const NOP: usize = 1000;
    // Biggest key in the hash-map
    const KEY_SPACE: usize = 10_000;
    // Key distribution
    const UNIFORM: &'static str = "uniform";
    //const SKEWED: &'static str = "skewed";
    // Read/Write ratio
    const WRITE_RATIO: usize = 10; //% out of 100

    let ops = generate_operations(NOP, WRITE_RATIO, KEY_SPACE, UNIFORM);
    mkbench::baseline_comparison::<NodeReplicated<NrHashMap>>(c, "hashmap", ops, LOG_SIZE_BYTES);
}

/// Compare scale-out behaviour of synthetic data-structure.
fn hashmap_scale_out<R>(c: &mut TestHarness, name: &str, write_ratio: usize, ops_prefix: &str, load_prefix: &str)
where
    R: DsInterface + Send + Sync + 'static,
    R::D: Send,
    R::D: Dispatch<ReadOperation<'static> = OpRd>,
    R::D: Dispatch<WriteOperation = OpWr>,
    <R::D as Dispatch>::WriteOperation: Send + Sync,
    <R::D as Dispatch>::ReadOperation<'static>: Send + Sync,
    <R::D as Dispatch>::Response: Sync + Send + Debug,
{
    let ops = generate_operations(NOP, write_ratio, KEY_SPACE, UNIFORM);

    let mut op_vec = Vec::new();
    let bench_name = format!("{}-scaleout-wr{}", name, write_ratio);

    if !ops_prefix.is_empty() {
        for i in 1..21 {
            let file_name = format!("{}{}", ops_prefix, i);
            match load_operations(&file_name) {
                Ok(operations) => {
                    op_vec.extend(operations);
                }
                Err(_e) => {
                    println!("Error loading operations");
                }
            }
        }

        if LOAD_PREFIX.is_empty() {
            panic!("LOAD operations cannot be empty with ycsb")
        }

        let mut load_vec = Vec::new();
        for i in 1..11 {
            let file_name = format!("{}{}", LOAD_PREFIX, i);
            match load_operations(&file_name) {
                Ok(operations) => {
                    load_vec.extend(operations);
                }
                Err(_e) => {
                    println!("Error loading operations");
                }
            }
        }

        let stripe_len = op_vec.len()/20;
        println!("op_vec length {}", stripe_len);
        mkbench::ScaleBenchBuilder::<R>::new(op_vec)
        .set_numa(NUMA)
        .thread_defaults()
        //.threads(1)
        //.threads(73)
        //.threads(96)
        //.threads(192)
        .update_batch(32)
        .log_size(32 * 1024 * 1024)
        .loadop_vec(load_vec)
        // .replica_strategy(mkbench::ReplicaStrategy::One)
        .replica_strategy(
            if NUMA { mkbench::ReplicaStrategy::NUMA } 
                else { mkbench::ReplicaStrategy::Socket }
            )
        .thread_mapping(
                if NUMA { ThreadMapping::NUMA }
                else { ThreadMapping::Interleave }
            )
        .log_strategy(mkbench::LogStrategy::One)
        .op_stripe_len(stripe_len)
        .configure(
            c,
            &bench_name,
            |_cid, tkn, replica, op: &Operation<OpRd, OpWr>, _batch_size| match op {
                Operation::ReadOperation(op) => {
                    replica.execute(*op, tkn);
                }
                Operation::WriteOperation(op) => {
                    replica.execute_mut(*op, tkn);
                }
            },
        );
    } else {
        mkbench::ScaleBenchBuilder::<R>::new(ops)
        .set_numa(NUMA)
        .thread_defaults()
        //.threads(1)
        //.threads(73)
        //.threads(96)
        //.threads(192)
        .update_batch(32)
        .log_size(32 * 1024 * 1024)
        // .replica_strategy(mkbench::ReplicaStrategy::One)
        .replica_strategy(
            if NUMA { mkbench::ReplicaStrategy::NUMA } 
                else { mkbench::ReplicaStrategy::Socket }
            )
        .thread_mapping(
                if NUMA { ThreadMapping::NUMA }
                else { ThreadMapping::Interleave }
            )
        .log_strategy(mkbench::LogStrategy::One)
        .configure(
            c,
            &bench_name,
            |_cid, tkn, replica, op: &Operation<OpRd, OpWr>, _batch_size| match op {
                Operation::ReadOperation(op) => {
                    replica.execute(*op, tkn);
                }
                Operation::WriteOperation(op) => {
                    replica.execute_mut(*op, tkn);
                }
            },
        );
    }    
}

/// Compare scale-out behaviour of partitioned hashmap data-structure.
// fn partitioned_hashmap_scale_out(c: &mut TestHarness, name: &str, write_ratio: usize) {
//     let ops = generate_operations(NOP, write_ratio, KEY_SPACE, UNIFORM);
//     let bench_name = format!("{}-scaleout-wr{}", name, write_ratio);

//     mkbench::ScaleBenchBuilder::<Partitioner<NrHashMap>>::new(ops)
//         .thread_defaults()
//         .replica_strategy(mkbench::ReplicaStrategy::PerThread)
//         .thread_mapping(ThreadMapping::Interleave)
//         .log_strategy(mkbench::LogStrategy::One)
//         .update_batch(128)
//         .configure(
//             c,
//             &bench_name,
//             |_cid, tkn, replica, op, _batch_size| match op {
//                 Operation::ReadOperation(op) => {
//                     replica.execute(*op, tkn).unwrap();
//                 }
//                 Operation::WriteOperation(op) => {
//                     replica.execute_mut(*op, tkn).unwrap();
//                 }
//             },
//         );
// }

// fn concurrent_ds_scale_out<T>(c: &mut TestHarness, name: &str, write_ratio: usize)
// where
//     T: Dispatch<ReadOperation<'static> = OpConcurrent>,
//     T: Dispatch<WriteOperation = ()>,
//     T: 'static,
//     T: Dispatch + Sync + Default + Send,
//     <T as Dispatch>::Response: Send + Sync + Debug,
// {
//     let ops = generate_operations_concurrent(NOP, write_ratio, KEY_SPACE, UNIFORM);
//     let bench_name = format!("{}-scaleout-wr{}", name, write_ratio);

//     mkbench::ScaleBenchBuilder::<ConcurrentDs<T>>::new(ops)
//         .thread_defaults()
//         .replica_strategy(mkbench::ReplicaStrategy::One) // Can only be One
//         .update_batch(128)
//         .thread_mapping(ThreadMapping::Interleave)
//         .log_strategy(mkbench::LogStrategy::One)
//         .configure(
//             c,
//             &bench_name,
//             |_cid, tkn, replica, op, _batch_size| match op {
//                 Operation::ReadOperation(op) => {
//                     replica.execute(*op, tkn);
//                 }
//                 Operation::WriteOperation(op) => {
//                     replica.execute_mut(*op, tkn);
//                 }
//             },
//         );
// }

fn main() {
    let _r = env_logger::try_init();
    if cfg!(feature = "smokebench") {
        warn!("Running with feature 'smokebench' may not get the desired results");
    }

    bench_utils::disable_dvfs();

    let mut harness = Default::default();

    let write_ratios = if cfg!(feature = "exhaustive") {
        vec![0, 10, 20, 40, 60, 80, 100]
    } else if cfg!(feature = "smokebench") {
        vec![10]
    } else {
        vec![0]
    };

    unsafe {
        urcu_sys::rcu_init();
    }

    //hashmap_single_threaded(&mut harness);
    for write_ratio in write_ratios.into_iter() {
        hashmap_scale_out::<NodeReplicated<NrHashMap>>(&mut harness, "hashmap", write_ratio,
            OPERATION_PREFIX, LOAD_PREFIX);

        // #[cfg(feature = "cmp")]
        // {
        //     partitioned_hashmap_scale_out(&mut harness, "partitioned-hashmap", write_ratio);
        //     concurrent_ds_scale_out::<CHashMapWrapper>(&mut harness, "chashmap", write_ratio);
        //     concurrent_ds_scale_out::<StdWrapper>(&mut harness, "std", write_ratio);
        //     concurrent_ds_scale_out::<FlurryWrapper>(&mut harness, "flurry", write_ratio);
        //     concurrent_ds_scale_out::<RcuHashMap>(&mut harness, "urcu", write_ratio);
        //     concurrent_ds_scale_out::<DashWrapper>(&mut harness, "dashmap", write_ratio);
        // }
    }
}
