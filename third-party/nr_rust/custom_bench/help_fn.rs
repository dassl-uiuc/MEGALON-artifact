use log::{debug, trace};

use std::fs::File;
use std::sync::Arc;
use std::io::{self, BufRead};
use std::path::Path;
use std::str::FromStr;
use std::collections::{HashMap, HashSet};
use zipf::ZipfDistribution;
use rand::seq::SliceRandom;
use rand::{distributions::Distribution, Rng, RngCore};

use node_replication::nr::{AffinityChange, NodeReplicated, ThreadToken};

use bench_utils::topology::*;

use crate::structs::*;

pub fn mean_128(data: &[u128]) -> Option<f64> {
    let sum = data.iter().sum::<u128>() as f64;
    let count = data.len();

    match count {
        positive if positive > 0 => Some(sum / count as f64),
        _ => Some(-1.0),
    }
}

pub fn mean_64(data: &[u64]) -> Option<f64> {
    let sum = data.iter().sum::<u64>() as f64;
    let count = data.len();

    match count {
        positive if positive > 0 => Some(sum / count as f64),
        _ => Some(-1.0),
    }
}

pub fn linux_chg_affinity(af: AffinityChange) -> usize {
    match af {
        AffinityChange::Replica(rid) => {
            let mut cpu: usize = 0;
            let mut node: usize = 0;
            unsafe { nix::libc::syscall(nix::libc::SYS_getcpu, &mut cpu, &mut node, 0) };

            let mut cpu_set = nix::sched::CpuSet::new();
            trace!(
                "cpus for node={} are {:#?}",
                rid,
                MACHINE_TOPOLOGY.cpus_on_node(rid as u64)
            );
            for ncpu in MACHINE_TOPOLOGY.cpus_on_node(rid as u64) {
                debug!("ncpu is {:?}", ncpu);
                cpu_set
                    .set(ncpu.cpu as usize)
                    .expect("Can't toggle CPU in cpu_set");
            }
            debug!(
                "we are on cpu {} node {} and should handle things for replica {} now, changing affinity to {:?}",
                cpu, node, rid, cpu_set
            );
            nix::sched::sched_setaffinity(nix::unistd::Pid::from_raw(0), &cpu_set)
                .expect("Can't change thread affinity");

            cpu as usize
        }

        AffinityChange::Revert(core_id) => {
            let mut cpu_set = nix::sched::CpuSet::new();
            cpu_set.set(core_id).expect("Can't toggle CPU in cpu_set");
            nix::sched::sched_setaffinity(nix::unistd::Pid::from_raw(0), &cpu_set)
                .expect("Can't reset thread affinity");
            0xdead
        }
    }
}

pub fn replica_core_allocation(
    topology: &MachineTopology,
    rs: ReplicaStrategy,
    tm: ThreadMapping,
    ts: usize,
) -> HashMap<usize, Vec<Cpu>> {
    let cpus = topology.allocate(tm, ts, true);
    debug_assert_eq!(ts, cpus.len());

    trace!(
        "Allocated cores for benchmark with {:?} {:?} {:?}",
        rs,
        tm,
        cpus
    );
    let mut rm: HashMap<usize, Vec<Cpu>> = HashMap::new();

    match rs {
        ReplicaStrategy::One => {
            rm.insert(0, cpus.iter().map(|c| c.cpu).collect());
        }
        ReplicaStrategy::Socket => {
            let mut sockets: Vec<Socket> = cpus.iter().map(|t| t.socket).collect();
            sockets.sort();
            sockets.dedup();

            for s in sockets {
                rm.insert(
                    s as usize,
                    cpus.iter()
                        .filter(|c| c.socket == s)
                        .map(|c| c.cpu)
                        .collect(),
                );
            }
        }
        ReplicaStrategy::L1 => match tm {
            ThreadMapping::None => {}
            ThreadMapping::Sequential => {
                let mut l1: Vec<L1> = cpus.iter().map(|t| t.l1).collect();
                l1.sort();
                l1.dedup();

                for s in l1 {
                    rm.insert(
                        s as usize,
                        cpus.iter().filter(|c| c.l1 == s).map(|c| c.cpu).collect(),
                    );
                }
            }
            // Giving replica number based on L1 number won't work in this case, as the
            // L1 numbers are allocated to Node-0 first and then to Node-1, and so on.
            ThreadMapping::Interleave => {
                let mut l1: Vec<L1> = cpus.iter().map(|t| t.l1).collect();
                l1.sort();
                l1.dedup();

                let mut rid = 0;
                let mut mapping: HashMap<L1, usize> = HashMap::with_capacity(cpus.len());
                for cpu in cpus.iter() {
                    let cache_num = cpu.l1;
                    if mapping.get(&cache_num).is_none() {
                        mapping.insert(cache_num, rid);
                        rid += 1;
                    }
                }

                for s in l1 {
                    rm.insert(
                        *mapping.get(&s).unwrap(),
                        cpus.iter().filter(|c| c.l1 == s).map(|c| c.cpu).collect(),
                    );
                }
            }
        },
        ReplicaStrategy::L2 => {
            let mut l2: Vec<L2> = cpus.iter().map(|t| t.l2).collect();
            l2.sort();
            l2.dedup();

            for s in l2 {
                rm.insert(
                    s as usize,
                    cpus.iter().filter(|c| c.l2 == s).map(|c| c.cpu).collect(),
                );
            }
        }
        ReplicaStrategy::L3 => {
            let mut l3: Vec<L3> = cpus.iter().map(|t| t.l3).collect();
            l3.sort();
            l3.dedup();

            for s in l3 {
                rm.insert(
                    s as usize,
                    cpus.iter().filter(|c| c.l3 == s).map(|c| c.cpu).collect(),
                );
            }
        }
        ReplicaStrategy::PerThread => {
            for (idx, core) in cpus.iter().map(|c| c.cpu).enumerate() {
                rm.insert(idx, vec![core]);
            }
        }
    };

    rm
}

pub fn concatenate_u64(value1: u64, value2: u64, value3: u64) -> [u8; 24] {
    let mut bytes = [0u8; 24];

    // Copy each u64 into the array
    bytes[0..8].copy_from_slice(&value1.to_be_bytes());
    bytes[8..16].copy_from_slice(&value2.to_be_bytes());
    bytes[16..24].copy_from_slice(&value3.to_be_bytes());

    bytes
}


pub fn generate_operations(
    nop: usize,
    write_ratio: usize,
    span: usize,
    distribution: &'static str,
) -> Vec<Ops> {
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
            key: concatenate_u64(id, id, id)
        };
        if idx % 100 < write_ratio {
            ops.push(Ops {
                opcode: OpCode::Put, 
                key: key, 
                val: t_rng.next_u64(),
            });
        } else {
            ops.push(Ops {
                opcode: OpCode::Get, 
                key: key, 
                val: 0,
            });
        }
    }

    ops.shuffle(&mut t_rng);
    ops
}

pub fn prepopulate(thread_token: ThreadToken, replica: &Arc<NodeReplicated<NrHashMap>>, key_space: usize, ratio: f64) {
    let num_key = (key_space as f64 * ratio) as usize;
    let mut keys: HashSet<GcdKey> = HashSet::new();
    let mut t_rng = rand::thread_rng();
    loop {
        let id = t_rng.gen_range(0..key_space as u64);
        let key: GcdKey = GcdKey{
            key: concatenate_u64(id, id, id)
        };
        if !keys.contains(&key) {
            keys.insert(key);
            if keys.len() >= num_key {
                break;
            }
        }
    }

    for key in &keys {
        replica.execute_mut(Modify::Put(*key, 0), thread_token);
    }
}

pub fn read_workload_file(filename: &str) -> io::Result<Vec<Ops>> {
    let mut operations = Vec::new();
    let path = Path::new(filename);
    let file = File::open(&path)?;
    let reader = io::BufReader::new(file);

    for line in reader.lines() {
        let line = line?;
        let parts: Vec<&str> = line.split_whitespace().collect();

        if parts.len() < 3 {
            continue; // Skip malformed lines
        }

        let opcode = OpCode::from_str(parts[0]).unwrap_or(OpCode::Put); // Default to Put if parsing fails
        let key = GcdKey::new(parts[1]);
        let val = parts[2].replace("field0=", ""); // Remove the "field0=" prefix

        operations.push(Ops::new(opcode, key, val));
    }

    Ok(operations)
}