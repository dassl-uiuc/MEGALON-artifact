use std::collections::HashMap;
use std::sync::Arc;
use std::str::FromStr;
use std::convert::TryInto;

use log::debug;
use serde::Serialize;

use node_replication::nr::{ThreadToken, Dispatch, NodeReplicated};

pub const NOP: usize = 25_000_000;
pub const KEY_SPACE: usize = 50_000_000;
pub const WRITE_RATIO: usize = 10;
pub const PREPOPULATE_RATIO: f64 = 0.3;
pub const LOAD_FACTOR: f64 = 0.1;
pub const BATCH_SIZE: u64 = 2048 * 1024;
pub const LOG_SIZE: usize = 32 * 1024 * 1024; // 16 MB
pub const UNIFORM: &'static str = "uniform";
pub const SKEWED: &'static str = "skewed";
pub const DIST: &'static str = UNIFORM;

#[derive(Serialize, Debug, Copy, Clone, Eq, PartialEq)]
pub enum ReplicaStrategy {
    /// One replica per system.
    One,
    /// One replica per L1 cache.
    L1,
    /// One replica per L2 cache.
    L2,
    /// One replica per L3 cache.
    L3,
    /// One replica per socket.
    Socket,
    /// One for every hardware thread.
    PerThread,
}

#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub struct GcdKey {
    pub key: [u8; 24],
}

impl GcdKey {
    pub fn new(key_str: &str) -> Self {
        let mut key = [0u8; 24];
        for (i, c) in key_str.chars().take(24).enumerate() {
            key[i] = c as u8;
        }
        GcdKey { key }
    }
}

/// The node-replicated hashmap uses a std hashmap internally.
pub struct NrHashMap {
    storage: HashMap<GcdKey, u64>,
}

impl Default for NrHashMap {
    fn default() -> Self {
        let cap: usize = (KEY_SPACE as f64 / LOAD_FACTOR) as usize;
        let storage = HashMap::with_capacity(cap);
        NrHashMap { storage }
    }
}

/// We support a mutable put operation to insert a value for a given key.
#[derive(Clone, Debug, PartialEq)]
pub enum Modify {
    /// Insert (key, value)
    Put(GcdKey, u64),
    CheckPut(GcdKey, u64),
    Delete(GcdKey),
}

/// We support an immutable read operation to lookup a key from the hashmap.
#[derive(Clone, Debug, PartialEq)]
pub enum Access {
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
            Modify::CheckPut(key, value) => {
                if !self.storage.contains_key(&key) {
                    return self.storage.insert(key, value)
                }
                None
            },
            Modify::Delete(key) => self.storage.remove(&key),
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub enum OpCode {
    Put,
    Get,
    CheckPut,
    Delete,
}

impl FromStr for OpCode {
    type Err = ();

    fn from_str(input: &str) -> Result<OpCode, Self::Err> {
        match input {
            "Put" => Ok(OpCode::Put),
            "Get" => Ok(OpCode::Get),
            "CheckPut" => Ok(OpCode::CheckPut),
            "Delete" => Ok(OpCode::Delete),
            _ => Err(()),
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub struct Ops {
    pub opcode: OpCode,
    pub key: GcdKey,
    pub val: u64,
}

impl Ops {
    pub fn new(opcode: OpCode, key: GcdKey, val_str: String) -> Self {
        let val_bytes = val_str.as_bytes();
        let val: u64;
        if val_bytes.len() != 8 {
            val = 0;
            debug!("Warning: Value string is not 8 bytes. Defaulting to 0.");
        } else {
            val = u64::from_le_bytes(val_bytes.try_into().unwrap());
        }
        Ops { opcode, key, val }
    }
}

pub type BenchFn = fn(
    idx: ThreadToken,
    replica: &Arc<NodeReplicated<NrHashMap>>,
    operation: &Ops,
);