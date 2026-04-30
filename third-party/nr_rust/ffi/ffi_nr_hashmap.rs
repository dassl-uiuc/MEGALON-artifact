use core_affinity;
use std::ffi::c_void;
use std::num::NonZeroUsize;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};

use node_replication::nr::{AffinityChange, Dispatch, NodeReplicated};

mod topology;
mod constants;
mod types;

use topology::*;
use constants::*;
use types::*;


// generator function: generate execution NUMA nodes skipping the memory node (NUMA_MEM)
const fn generate_array() -> [u64; NUM_EXEC] {
    let mut arr = [0; NUM_EXEC];
    let mut filled = 0;
    let mut candidate: u64 = 0;
    while filled < NUM_EXEC {
        if candidate == NUMA_MEM {
            candidate += 1;
            continue;
        }
        arr[filled] = candidate;
        filled += 1;
        candidate += 1;
    }
    arr
}
const NUMA_EXEC: [u64; NUM_EXEC] = generate_array();

/// We support a mutable put operation to insert a value for a given key.
#[derive(Clone, Debug, PartialEq)]
pub enum Modify {
    Put(BlockId, GCDEntry),
    CheckPut(BlockId, GCDEntry),
    Delete(BlockId),
    Swap(BlockId, BlockId, GCDEntry),
    PutArray(BlockId, isize, u64),
    PutArrayWmeta(BlockId, isize, u64, isize),
    CheckPutArray(BlockId, isize, u64),
    CheckPutArrayWmeta(BlockId, isize, u64, isize),
    CheckPutSwapArray(BlockId, isize, u64, u64),
    InvalidateArray(BlockId, u64),
    InvalidateExceptArray(BlockId, u64),
    InvalidateExceptArrayUpdateWmeta(BlockId, u64, isize),
    CheckMoveLocalArray(BlockId, isize, u64),
    DeleteArray(BlockId, u64),
    DeleteIfArray(BlockId, u64),
    CheckSwapArray(BlockId, BlockId, isize, u64, u64),

    CheckSwitchWmetaArray(BlockId, isize),

    DummyLog(BlockId),
}

/// We support an immutable read operation to lookup a key from the hashmap.
#[derive(Clone, Debug, PartialEq)]
pub enum Access {
    Get(BlockId),
}

fn linux_chg_affinity(af: AffinityChange) -> usize {
    match af {
        AffinityChange::Replica(rid) => {
            let mut cpu: usize = 0;
            let mut node: usize = 0;
            unsafe { nix::libc::syscall(nix::libc::SYS_getcpu, &mut cpu, &mut node, 0) };

            let mut cpu_set = nix::sched::CpuSet::new();
            for ncpu in MACHINE_TOPOLOGY.cpus_on_node(rid as u64) {
                cpu_set
                    .set(ncpu.cpu as usize)
                    .expect("Can't toggle CPU in cpu_set");
            }
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

fn linux_chg_affinity_numa(af: AffinityChange) -> usize {
    match af {
        AffinityChange::Replica(rid) => {
            let mut cpu: usize = 0;
            let mut node: usize = 0;
            unsafe { nix::libc::syscall(nix::libc::SYS_getcpu, &mut cpu, &mut node, 0) };

            let mut cpu_set = nix::sched::CpuSet::new();
            let rid_numa: u64 = if rid == std::usize::MAX {
                NUMA_MEM
            } else {
                NUMA_EXEC[rid % NUM_EXEC]
            };

            for ncpu in MACHINE_TOPOLOGY.cpus_on_node(rid_numa) {
                cpu_set
                    .set(ncpu.cpu as usize)
                    .expect("Can't toggle CPU in cpu_set");
            }
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

/// The Dispatch trait executes `ReadOperation` (our Access enum) and `WriteOperation`
/// (our `Modify` enum) against the replicated data-structure.
impl Dispatch for NrHashMapFFi {
    type ReadOperation<'rop> = Access;
    type WriteOperation = Modify;
    type Response = OptionGCDReponse;

    /// The `dispatch` function contains the logic for the immutable operations.
    fn dispatch<'rop>(&self, op: Self::ReadOperation<'rop>) -> Self::Response {
        match op {
            Access::Get(key) => OptionGCDReponse(
                self.storage.get(&key).map(|&value| GCDReponse {
                    key: key,
                    value: value,
                })),
        }
    }

    /// The `dispatch_mut` function contains the logic for the mutable operations.
    fn dispatch_mut(&mut self, op: Self::WriteOperation) -> Self::Response {
        match op {
            Modify::Put(key, value) => {
                self.storage.insert(key, value);
                return OptionGCDReponse(Some(GCDReponse {
                    key: key,
                    value: value,
                }));
            }
            Modify::CheckPut(key, value) => {
                if !self.storage.contains_key(&key) {
                    self.storage.insert(key, value);
                    return OptionGCDReponse(Some(GCDReponse {
                        key: key,
                        value: value,
                    }));
                }
                OptionGCDReponse(None)
            }
            Modify::Delete(key) => {
                if let Some(entry) = self.storage.get_mut(&key) {
                    let old_wmeta_index = entry.wmeta_idx_;
                    self.storage.remove(&key);
                    return OptionGCDReponse(Some(GCDReponse {
                        key: key,
                        value: init_empty_gcd_entry_with_wmeta_idx(old_wmeta_index), // this is needed to record in Change Notification
                    }));
                }
                OptionGCDReponse(None)
            }
            Modify::Swap(to_remove, to_insert, value) => {
                self.storage.remove(&to_remove);
                self.storage.insert(to_insert, value);
                OptionGCDReponse(None)
            }

            Modify::PutArray(key, cn, idx) => {
                if !self.storage.contains_key(&key) {
                    let value = init_gcd_entry(cn, idx);
                    self.storage.insert(key, value);
                    return OptionGCDReponse(Some(GCDReponse {
                        key: key,
                        value: value,
                    }));
                } else {
                    let entry = self.storage.get_mut(&key).unwrap();
                    entry.cn_array_[idx as usize] = CNStatus {
                        cn_idx_: cn,
                        invalidate_: false,
                    };
                    return OptionGCDReponse(Some(GCDReponse {
                        key: key,
                        value: *entry,
                    }));
                };
            }
            Modify::CheckPutArray(key, cn, idx) => {
                if !self.storage.contains_key(&key) {
                    let value = init_gcd_entry(cn, idx);
                    self.storage.insert(key, value);
                    return OptionGCDReponse(Some(GCDReponse {
                        key: key,
                        value: value,
                    }));
                }
                let entry = self.storage.get_mut(&key).unwrap();
                if entry.cn_array_[idx as usize].cn_idx_ == -1 {
                    entry.cn_array_[idx as usize] = CNStatus {
                        cn_idx_: cn,
                        invalidate_: false,
                    };
                    return OptionGCDReponse(Some(GCDReponse {
                        key: key,
                        value: *entry,
                    }));
                }
                OptionGCDReponse(None)
            }
            Modify::PutArrayWmeta(key, cn, idx, wmeta_idx) => {
                if !self.storage.contains_key(&key) {
                    let mut value = init_gcd_entry(cn, idx);
                    value.wmeta_idx_ = wmeta_idx;
                    self.storage.insert(key, value);
                    return OptionGCDReponse(Some(GCDReponse {
                        key: key,
                        value: value,
                    }));
                } else {
                    let entry = self.storage.get_mut(&key).unwrap();
                    entry.cn_array_[idx as usize] = CNStatus {
                        cn_idx_: cn,
                        invalidate_: false,
                    };
                    entry.wmeta_idx_ = wmeta_idx;
                    return OptionGCDReponse(Some(GCDReponse {
                        key: key,
                        value: *entry,
                    }));
                };
            }
            Modify::CheckPutArrayWmeta(key, cn, idx, wmeta_idx) => {
                if !self.storage.contains_key(&key) {
                    let mut value = init_gcd_entry(cn, idx);
                    if idx == NUMA_MEM {
                        value.wmeta_idx_ = wmeta_idx;
                    }
                    self.storage.insert(key, value);
                    return OptionGCDReponse(Some(GCDReponse {
                        key: key,
                        value: value,
                    }));
                }
                let entry = self.storage.get_mut(&key).unwrap();
                if entry.cn_array_[idx as usize].cn_idx_ == -1 {
                    if entry.cn_array_[idx as usize].invalidate_ == true {
                        // fail the put if invalidate_ bit is set for the entry
                        panic!("BUG: Attempted to put into an invalidated entry at idx = {}", idx);
                    }
                    if entry.wmeta_idx_ != -1 && idx != NUMA_MEM {
                        // setting a local replica while in RW shared
                        panic!("BUG: Attempted to add local replica {} while wmeta valid: {}", idx, entry.wmeta_idx_);
                    }
                    entry.cn_array_[idx as usize] = CNStatus {
                        cn_idx_: cn,
                        invalidate_: false,
                    };
                    if idx == NUMA_MEM {
                        entry.wmeta_idx_ = wmeta_idx;
                    }
                    return OptionGCDReponse(Some(GCDReponse {
                        key: key,
                        value: *entry,
                    }));
                } else if idx == NUMA_MEM {
                    // slot is allocated, trying to switch rw mode
                    if wmeta_idx == -1 {
                        // switching to ro, idempotent
                        entry.wmeta_idx_ = wmeta_idx;
                        return OptionGCDReponse(Some(GCDReponse {
                            key: null_BlockId(),
                            value: *entry,
                        }));
                    } else if entry.wmeta_idx_ == -1 {
                        // only update wmeta_idx_ if it is not already set
                        entry.wmeta_idx_ = wmeta_idx;
                        return OptionGCDReponse(Some(GCDReponse {
                            key: null_BlockId(),
                            value: *entry,
                        }));
                    }
                    return OptionGCDReponse(None); // neither set slot and set wmeta is successful
                }
                OptionGCDReponse(None)
            }
            Modify::CheckSwitchWmetaArray(key, wmeta_idx) => {
                if self.storage.contains_key(&key) {
                    // slot already exists, check if we can update wmeta
                    let entry = self.storage.get_mut(&key).unwrap();
                    if wmeta_idx == -1 {
                        // switching to ro, idempotent
                        let mut dummy = init_empty_gcd_entry();
                        dummy.wmeta_idx_ = entry.wmeta_idx_;
                        entry.wmeta_idx_ = wmeta_idx;
                        return OptionGCDReponse(Some(GCDReponse {
                            key: key,
                            value: dummy,
                        }));
                    } else if entry.wmeta_idx_ == -1 {
                        // only update wmeta_idx_ if it is not already set
                        entry.wmeta_idx_ = wmeta_idx;
                        return OptionGCDReponse(Some(GCDReponse {
                            key: key,
                            value: *entry,
                        }));
                    }
                }
                // either the entry does not exist or try double switch to rw
                OptionGCDReponse(None)
            }
            Modify::CheckPutSwapArray(key, cn, new, old) => {
                if !self.storage.contains_key(&key) {
                    let value = init_gcd_entry(cn, new);
                    self.storage.insert(key, value);
                    return OptionGCDReponse(Some(GCDReponse {
                        key: key,
                        value: value,
                    }));
                }
                let entry = self.storage.get_mut(&key).unwrap();
                entry.cn_array_[old as usize] = null_CNStatus();
                if entry.cn_array_[new as usize].cn_idx_ == -1 {
                    entry.cn_array_[new as usize] = CNStatus {
                        cn_idx_: cn,
                        invalidate_: false,
                    };
                    return OptionGCDReponse(Some(GCDReponse {
                        key: key,
                        value: *entry,
                    }));
                }
                OptionGCDReponse(None)
            }
            Modify::InvalidateArray(key, idx) => {
                if let Some(entry) = self.storage.get_mut(&key) {
                    if entry.cn_array_[idx as usize].cn_idx_ != -1 {
                        entry.cn_array_[idx as usize].invalidate_ = true;
                    }
                }
                OptionGCDReponse(None)
            }
            Modify::InvalidateExceptArray(key, idx) => {
                if let Some(entry) = self.storage.get_mut(&key) {
                    for i in 0..LOGICAL_NODE_NUM + 1 {
                        if entry.cn_array_[i as usize].cn_idx_ != -1 && idx != i as u64 {
                            entry.cn_array_[i as usize].invalidate_ = true;
                        }
                    }
                }
                OptionGCDReponse(None)
            }
            Modify::InvalidateExceptArrayUpdateWmeta(key, idx, wmeta_idx) => {
                if let Some(entry) = self.storage.get_mut(&key) {
                    for i in 0..LOGICAL_NODE_NUM + 1 {
                        if entry.cn_array_[i as usize].cn_idx_ != -1 && idx != i as u64 {
                            entry.cn_array_[i as usize].invalidate_ = true;
                        }
                    }
                    if entry.wmeta_idx_ == -1 {
                        entry.wmeta_idx_ = wmeta_idx;
                        return OptionGCDReponse(Some(GCDReponse {
                            key: key,
                            value: *entry,
                        }));
                    } else {
                        return OptionGCDReponse(Some(GCDReponse {
                            key: null_BlockId(),
                            value: init_empty_gcd_entry(),
                        }));
                    }
                }
                OptionGCDReponse(None)
            }
            Modify::CheckMoveLocalArray(key, cn, idx) => {
                // assumes index 0 is cxl replica
                if idx == 0 {
                    panic!("BUG: cannot move to cxl");
                }
                if !self.storage.contains_key(&key) {
                    panic!("BUG: the key does not exist");
                }
                let entry = self.storage.get_mut(&key).unwrap();
                if entry.cn_array_[idx as usize].cn_idx_ == -1 || entry.cn_array_[idx as usize].invalidate_ == true {
                    entry.cn_array_[idx as usize] = CNStatus {
                        cn_idx_: cn,
                        invalidate_: false,
                    };
                    entry.wmeta_idx_ = -1;
                    let val = entry.cn_array_[0].cn_idx_;
                    entry.cn_array_[0] = null_CNStatus();
                    return OptionGCDReponse(Some(GCDReponse {
                        key: key,
                        value: init_gcd_entry(val, 0),
                    }));
                }
                OptionGCDReponse(None)
            }
            Modify::DeleteArray(key, idx) => {
                if let Some(entry) = self.storage.get_mut(&key) {
                    let val = entry.cn_array_[idx as usize].cn_idx_;
                    entry.cn_array_[idx as usize] = null_CNStatus();

                    if entry.cn_array_.iter().all(|status| status.cn_idx_ == -1) {
                        self.storage.remove(&key);
                    }
                    return OptionGCDReponse(Some(GCDReponse {
                        key: key,
                        value: init_gcd_entry(val, NUMA_MEM as u64),
                    }));
                }
                OptionGCDReponse(None)
            }

            Modify::DeleteIfArray(key, idx) => {
                if let Some(entry) = self.storage.get_mut(&key) {
                    if idx == NUMA_MEM {
                        // delete cxl replica
                        if entry.wmeta_idx_ != -1 {
                            // in RW shared, cannot delete
                            return OptionGCDReponse(Some(GCDReponse {
                                key: null_BlockId(),
                                value: init_gcd_entry(2, NUMA_MEM as u64 + 1), // 2 is magic number
                            }));
                        }
                        if entry.cn_array_[idx as usize] == null_CNStatus() {
                            // cxl replica is already deleted
                            return OptionGCDReponse(Some(GCDReponse {
                                key: null_BlockId(),
                                value: init_empty_gcd_entry(),
                            }))
                        }
                    } else {
                        // delete local replica
                        if entry.cn_array_[idx as usize] == null_CNStatus() {
                            // local replica is already deleted
                            return OptionGCDReponse(Some(GCDReponse {
                                key: null_BlockId(),
                                value: init_empty_gcd_entry(),
                            }));
                        }
                        if entry.cn_array_[idx as usize].invalidate_ != true && 
                                entry.wmeta_idx_ != -1 {
                            panic!("BUG: local replica still valid idx = {} when page in RW shared", idx);
                        }
                    }

                    // sanity check pass, delete the replica
                    let val = entry.cn_array_[idx as usize].cn_idx_;
                    entry.cn_array_[idx as usize] = null_CNStatus();

                    // check if entire entry need delete
                    if entry.cn_array_.iter().all(|status| status.cn_idx_ == -1) {
                        self.storage.remove(&key);
                    }

                    return OptionGCDReponse(Some(GCDReponse {
                        key: key,
                        value: init_gcd_entry(val, NUMA_MEM as u64), // this is needed to record in Change Notification
                    }));
                }
                OptionGCDReponse(None)
            }

            Modify::CheckSwapArray(old_key, new_key, cn, old_idx, new_idx) => {
                // Modify::DeleteArray(old_key, old_idx)
                if let Some(entry) = self.storage.get_mut(&old_key) {
                    entry.cn_array_[old_idx as usize] = null_CNStatus();

                    if entry.cn_array_.iter().all(|status| status.cn_idx_ == -1) {
                        self.storage.remove(&old_key);
                    }
                } else {
                    return OptionGCDReponse(None);
                }
                // Modify::CheckPutArray(new_key, cn, new_idx)
                if !self.storage.contains_key(&new_key) {
                    let value = init_gcd_entry(cn, new_idx);
                    self.storage.insert(new_key, value);
                    return OptionGCDReponse(Some(GCDReponse {
                        key: new_key,
                        value: value,
                    }));
                }
                let entry = self.storage.get_mut(&new_key).unwrap();
                if entry.cn_array_[new_idx as usize].cn_idx_ == -1 {
                    entry.cn_array_[new_idx as usize] = CNStatus {
                        cn_idx_: cn,
                        invalidate_: false,
                    };
                    return OptionGCDReponse(Some(GCDReponse {
                        key: new_key,
                        value: *entry,
                    }));
                }
                OptionGCDReponse(None)
            }

            Modify::DummyLog(key) => {
                if let Some(entry) = self.storage.get_mut(&key) {
                    return OptionGCDReponse(Some(GCDReponse {
                        key: key,
                        value: *entry,
                    }));
                }
                OptionGCDReponse(None)
            }
        }
    }
}

pub fn pin_thread(core_id: usize) {
    core_affinity::set_for_current(core_affinity::CoreId { id: core_id });
}

#[no_mangle]
pub extern "C" fn create_node_replicated(
    cap: NonZeroUsize,
    num_replica: NonZeroUsize,
) -> *mut Mutex<NrRapper> {
    let a = NrHashMapFFi::new(usize::from(cap));

    let nrht = Arc::new(
        NodeReplicated::<NrHashMapFFi>::new_with_data(num_replica, linux_chg_affinity, a).unwrap(),
    );

    // let nrht = Arc::new(NodeReplicated::<NrHashMapFFi>::new_with_data(
    //     num_replica, | _rid| 0, a).unwrap());

    let stop_signal = Arc::new(AtomicBool::new(false));

    let boxed_nrht = Box::new(Mutex::new(NrRapper {
        nrht: nrht,
        thread_handle: None,
        stop_signal: stop_signal,
    }));
    Box::into_raw(boxed_nrht)
}

#[no_mangle]
pub extern "C" fn create_node_replicated_numa(
    cap: NonZeroUsize,
    num_replica: NonZeroUsize,
    base_addr: *const u8,
    slot_size: usize,
    numa_mem: u64,
    numa_num: usize,
) -> *mut Mutex<NrRapper> {
    let a = NrHashMapFFi::new(usize::from(cap));

    // sanity checks
    assert!(usize::from(num_replica) == LOGICAL_NODE_NUM);

    assert!(numa_mem == NUMA_MEM);

    assert!(numa_num == NUM_EXEC + 1);

    let nrht = Arc::new(
        NodeReplicated::<NrHashMapFFi>::new_with_data_and_base_addr_numa(
            num_replica, linux_chg_affinity_numa, a, base_addr, slot_size)
            .unwrap(),
    );

    let stop_signal = Arc::new(AtomicBool::new(false));

    let boxed_nrht = Box::new(Mutex::new(NrRapper {
        nrht: nrht,
        thread_handle: None,
        stop_signal: stop_signal,
    }));
    Box::into_raw(boxed_nrht)
}

// #[no_mangle]
// pub extern "C" fn create_node_replicated_numa_async(
//     cap: NonZeroUsize,
//     num_replica: NonZeroUsize,
//     bground_sync_period: u64,
// ) -> *mut Mutex<NrRapper> {
//     // creates log on socket 0, 2 replicas on socket 1
//     let a = NrHashMapFFi::new(usize::from(cap));

//     let nrht = Arc::new(
//         NodeReplicated::<NrHashMapFFi>::new_with_data_numa(num_replica, linux_chg_affinity_numa, a)
//             .unwrap(),
//     );

//     let stop_signal = Arc::new(AtomicBool::new(false));
//     let thread_stop_signal = Arc::clone(&stop_signal);
//     let nrht_thread = nrht.clone();

//     // Create and start the thread
//     let thread_handle = thread::spawn(move || {
//         pin_thread(1);
//         // println!("Background sync thread starts with period {} us...", bground_sync_period);
//         while !thread_stop_signal.load(Ordering::SeqCst) {
//             nrht_thread.log_sync_numa();
//             thread::sleep(std::time::Duration::from_micros(bground_sync_period));
//         }
//     });

//     let boxed_nrht = Box::new(Mutex::new(NrRapper {
//         nrht: nrht,
//         thread_handle: Some(thread_handle),
//         stop_signal: stop_signal,
//     }));
//     Box::into_raw(boxed_nrht)
// }

#[no_mangle]
pub extern "C" fn free_node_replicated(nrht_wrapper: *mut Mutex<NrRapper>) {
    if nrht_wrapper.is_null() {
        return;
    }
    unsafe {
        let boxed_nrht = Box::from_raw(nrht_wrapper);
        let mut nrht = boxed_nrht.lock().unwrap();

        if let Some(handle) = nrht.thread_handle.take() {
            nrht.stop_signal.store(true, Ordering::SeqCst);
            handle.join().unwrap();
        }
    }
}

// #[no_mangle]
// pub extern "C" fn register_data_base_addr(nrht_wrapper: *mut Mutex<NrRapper>, addr: *mut u8) {
//     unsafe {
//         let nrht_wrapper = &*nrht_wrapper;
//         let nrht = nrht_wrapper.lock().unwrap();

//         let nrht_clone = Arc::clone(&nrht.nrht);

//         unsafe {
//             nrht_clone.set_base_addr(addr);
//         }
//     }
// }

#[no_mangle]
pub extern "C" fn register_node_replicated(
    nrht_wrapper: *mut Mutex<NrRapper>,
    idx: usize,
) -> *mut NrMeta {
    unsafe {
        if nrht_wrapper.is_null() {
            return std::ptr::null_mut();
        }

        let nrht_wrapper = &*nrht_wrapper;
        let nrht = nrht_wrapper.lock().unwrap();

        let nrht_clone = Arc::clone(&nrht.nrht);

        let log_token = nrht_clone
            .register(idx)
            .expect(format!("Unable to register thread with replica {}", idx).as_str());

        let metadata = NrMeta {
            nrht: nrht_clone,
            ttkn: log_token,
        };

        Box::into_raw(Box::new(metadata))
    }
}

#[no_mangle]
pub extern "C" fn unregister_node_replicated(metadata: *mut NrMeta) {
    if metadata.is_null() {
        return;
    }
    unsafe {
        let _ = Box::from_raw(metadata);
    }
}

#[no_mangle]
pub extern "C" fn Put(metadata: *mut NrMeta, key: BlockId, val: GCDEntry) {
    unsafe {
        (*metadata)
            .nrht
            .execute_mut(Modify::Put(key, val), (*metadata).ttkn);
    }
}

#[no_mangle]
pub extern "C" fn PutArray(
    metadata: *mut NrMeta,
    key: BlockId,
    cn: isize,
    idx: u64,
    wmeta_idx: isize,
) {
    unsafe {
        (*metadata).nrht.execute_mut(
            Modify::PutArrayWmeta(key, cn, idx, wmeta_idx),
            (*metadata).ttkn,
        );
    }
}

#[no_mangle]
pub extern "C" fn Delete(metadata: *mut NrMeta, key: BlockId) -> isize {
    unsafe {
        let wrapper = (*metadata)
            .nrht
            .execute_mut(Modify::Delete(key), (*metadata).ttkn);
        let response = wrapper.0;
        match response {
            Some(value) => value.value.wmeta_idx_,
            None => -2,
        }
    }
}

#[no_mangle]
pub extern "C" fn DeleteArray(metadata: *mut NrMeta, key: BlockId, idx: u64) -> bool {
    unsafe {
        let wrapper = (*metadata)
            .nrht
            .execute_mut(Modify::DeleteArray(key, idx), (*metadata).ttkn);
        let response = wrapper.0;
        match response {
            Some(_) => true,
            None => false,
        }
    }
}

/**
 * delete replica if the entry exists and if delete from cxl copy, page is in RO
 *   0: no error
 *   1: wmeta valid
 *   2: entry does not exist
 *   3: the replica is already deleted on the idx
 */
#[no_mangle]
pub extern "C" fn DeleteIfArray(metadata: *mut NrMeta, key: BlockId, idx: u64) -> usize {
    unsafe {
        let wrapper = (*metadata)
            .nrht
            .execute_mut(Modify::DeleteIfArray(key, idx), (*metadata).ttkn);
        let response = wrapper.0;
        match response {
            Some(value) => {
                if value.key == null_BlockId() {
                    if value.value == init_empty_gcd_entry() {
                        return 3; // entry is already deleted
                    }
                    return 1; // wmeta valid
                }
                return 0;
            }
            None => 2,
        }
    }
}

#[no_mangle]
pub extern "C" fn InvalidateArray(metadata: *mut NrMeta, key: BlockId, idx: u64) {
    unsafe {
        (*metadata)
            .nrht
            .execute_mut(Modify::InvalidateArray(key, idx), (*metadata).ttkn);
    }
}

#[no_mangle]
pub extern "C" fn InvalidateExceptArray(metadata: *mut NrMeta, key: BlockId, idx: u64) {
    unsafe {
        (*metadata)
            .nrht
            .execute_mut(Modify::InvalidateExceptArray(key, idx), (*metadata).ttkn);
    }
}

/**
 * invalidate replica entries & set page to rw by updating valid wmeta
 *   0: no error
 *   1: entry does not exist
 *   2: wmeta already allocated
 */
#[no_mangle]
pub extern "C" fn InvalidateExceptArrayUpdateWmeta(
    metadata: *mut NrMeta,
    key: BlockId,
    idx: u64,
    wmeta_idx: isize,
) -> usize {
    unsafe {
        let wrapper = (*metadata).nrht.execute_mut(
            Modify::InvalidateExceptArrayUpdateWmeta(key, idx, wmeta_idx),
            (*metadata).ttkn,
        );
        let response = wrapper.0;
        match response {
            Some(value) => {
                if value.key == null_BlockId() {
                    return 2; // update wmeta unsuccessful
                }
                return 0;
            }
            None => 1,
        }
    }
}

#[no_mangle]
pub extern "C" fn CheckPut(metadata: *mut NrMeta, key: BlockId, val: GCDEntry) -> bool {
    unsafe {
        let wrapper = (*metadata)
            .nrht
            .execute_mut(Modify::CheckPut(key, val), (*metadata).ttkn);
        let response = wrapper.0;
        match response {
            Some(_) => true,
            None => false,
        }
    }
}

/**
 * return value:
 *   0: no error
 *   1: slot & wmeta update unsuccessful
 *   2: only slot update unsuccessful
 */
#[no_mangle]
pub extern "C" fn CheckPutArray(
    metadata: *mut NrMeta,
    key: BlockId,
    cn: isize,
    idx: u64,
    wmeta_idx: isize,
) -> usize {
    unsafe {
        let wrapper = (*metadata).nrht.execute_mut(
            Modify::CheckPutArrayWmeta(key, cn, idx, wmeta_idx),
            (*metadata).ttkn,
        );
        let response = wrapper.0;
        match response {
            Some(value) => {
                if value.key == null_BlockId() {
                    return 2; // update wmeta unsuccessful
                }
                return 0;
            }
            None => 1, // update wmeta & slot unsuccessful
        }
    }
}

/**
 * change the wemta of an entry, with failure condition:
 *   condition 1: entry does not exist
 *   condition 2: wmeta is already set (not -1)
 */
#[no_mangle]
pub extern "C" fn CheckSwitchWmetaArray(
    metadata: *mut NrMeta,
    key: BlockId,
    wmeta_idx: isize,
) -> isize {
    unsafe {
        let wrapper = (*metadata).nrht.execute_mut(
            Modify::CheckSwitchWmetaArray(key, wmeta_idx),
            (*metadata).ttkn,
        );
        let response = wrapper.0;
        match response {
            Some(value) => value.value.wmeta_idx_,
            None => -1,
        }
    }
}

/**
 * Move index to local exclusive, remove the cxl entry
 * return:
 *     0: successful,
 *     -1: fail (local entry already exists)
 */
#[no_mangle]
pub extern "C" fn CheckMoveLocalArray(
    metadata: *mut NrMeta,
    key: BlockId,
    cn: isize,
    idx: u64,
) -> isize {
    unsafe {
        let wrapper = (*metadata).nrht.execute_mut(
            Modify::CheckMoveLocalArray(key, cn, idx),
            (*metadata).ttkn,
        );
        let response = wrapper.0;
        match response {
            Some(_) => 0,
            None => -1,
        }
    }
}

#[no_mangle]
pub extern "C" fn CheckPutSwapArray(
    metadata: *mut NrMeta,
    key: BlockId,
    cn: *mut c_void,
    new_idx: u64,
    old_idx: u64,
) -> bool {
    unsafe {
        let wrapper = (*metadata).nrht.execute_mut(
            Modify::CheckPutSwapArray(key, cn as isize, new_idx, old_idx),
            (*metadata).ttkn,
        );
        let response = wrapper.0;
        match response {
            Some(_) => true,
            None => false,
        }
    }
}

#[no_mangle]
pub extern "C" fn Swap(
    metadata: *mut NrMeta,
    to_remove: BlockId,
    to_insert: BlockId,
    val: GCDEntry,
) -> bool {
    unsafe {
        (*metadata)
            .nrht
            .execute_mut(Modify::Swap(to_remove, to_insert, val), (*metadata).ttkn);
        true
    }
}

#[no_mangle]
pub extern "C" fn CheckSwapArray(
    metadata: *mut NrMeta,
    to_remove: BlockId,
    to_insert: BlockId,
    cn: isize,
    new_idx: u64,
    old_idx: u64,
) -> bool {
    unsafe {
        let wrapper = (*metadata).nrht.execute_mut(
            Modify::CheckSwapArray(to_remove, to_insert, cn, new_idx, old_idx),
            (*metadata).ttkn,
        );
        let response = wrapper.0;
        match response {
            Some(_) => true,
            None => false,
        }
    }
}

#[no_mangle]
pub extern "C" fn Get(metadata: *mut NrMeta, key: BlockId) -> GCDEntry {
    unsafe {
        let wrapper = (*metadata).nrht.execute(Access::Get(key), (*metadata).ttkn);
        let response = wrapper.0;
        match response {
            Some(value) => value.value,
            None => init_empty_gcd_entry(),
        }
    }
}

#[no_mangle]
pub extern "C" fn GetAnchor(metadata: *mut NrMeta, key: BlockId) -> GCDEntry {
    unsafe {
        let wrapper = (*metadata).nrht.execute_anchor(Access::Get(key), (*metadata).ttkn);
        let response = wrapper.0;
        match response {
            Some(value) => value.value,
            None => init_empty_gcd_entry(),
        }
    }
}

#[no_mangle]
pub extern "C" fn GetAsync(metadata: *mut NrMeta, key: BlockId) -> GCDEntry {
    unsafe {
        let wrapper = (*metadata)
            .nrht
            .execute_loose(Access::Get(key), (*metadata).ttkn);
        let response = wrapper.0;
        match response {
            Some(value) => value.value,
            None => init_empty_gcd_entry(),
        }
    }
}

#[no_mangle]
pub extern "C" fn CheckCoherence(metadata: *mut NrMeta, key: BlockId) -> bool {
    unsafe {
        let wrapper = (*metadata).nrht.execute(Access::Get(key), (*metadata).ttkn);
        let response = wrapper.0;
        match response {
            Some(value) => {
                if value.value.cn_array_[0].cn_idx_ == -1 {
                    return false;
                }
                let ret = (*metadata).nrht.check_coherence(wrapper, (*metadata).ttkn);
                ret
            }
            None => false,
        }
    }
}

#[no_mangle]
pub extern "C" fn CheckCoherenceSlot(metadata: *mut NrMeta, key: isize) -> bool {
    unsafe {
        if key == -1 {
            return false;
        }
        let wrapper = OptionGCDReponse(Some(GCDReponse {
            key: null_BlockId(),
            value: init_gcd_entry(key, NUMA_MEM),
        }));
        (*metadata).nrht.check_coherence(wrapper, (*metadata).ttkn)
    }
}

#[no_mangle]
pub extern "C" fn ResetCoherence(metadata: *mut NrMeta, key: BlockId) {
    unsafe {
        let wrapper = (*metadata).nrht.execute(Access::Get(key), (*metadata).ttkn);
        let response = wrapper.0;
        if response.is_some() {
            (*metadata).nrht.reset_coherence(wrapper, (*metadata).ttkn);
        }
    }
}

#[no_mangle]
pub extern "C" fn CheckCoherenceReset(metadata: *mut NrMeta, key: BlockId) -> bool {
    unsafe {
        let wrapper = (*metadata).nrht.execute(Access::Get(key), (*metadata).ttkn);
        let response = wrapper.0;
        match response {
            Some(_) => {
                let ret = (*metadata)
                    .nrht
                    .check_coherence_reset(wrapper, (*metadata).ttkn);
                ret
            }
            None => true,
        }
    }
}

#[no_mangle]
pub extern "C" fn CheckNotificationReset(metadata: *mut NrMeta) -> bool {
    unsafe {
        (*metadata).nrht.check_notification_reset((*metadata).ttkn)
    }
}

#[no_mangle]
pub extern "C" fn DummyEvent(metadata: *mut NrMeta, key: BlockId) {
    unsafe {
        let _ = (*metadata).nrht.execute_mut(
            Modify::DummyLog(key),
            (*metadata).ttkn,
        );
    }
}
