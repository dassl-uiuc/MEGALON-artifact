use std::hash::{Hash, Hasher};
#[cfg(debug_assertions)]
use std::io::{stdout, Write};

use node_replication::nr::IdxGetter;

use crate::constants::*;

// Type aliases
pub type ino_t = u64;
pub type off_t = i64;

// CNStatus struct
#[repr(C)]
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub struct CNStatus {
    pub cn_idx_: isize,
    pub invalidate_: bool,
}

pub const fn null_CNStatus() -> CNStatus {
    CNStatus {
        cn_idx_: -1,
        invalidate_: false,
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub struct SeqLock {
    pub lock_: bool,
    pub seqcount_: isize,
}

pub const fn null_SeqLock() -> SeqLock {
    SeqLock {
        lock_: false,
        seqcount_: 0,
    }
}

#[repr(C)]
pub struct TrySeqLockResult {
    pub entry_: GCDEntry,
    pub status_: isize,
}

// GCDEntry struct
// the first element in array is cxl mem, assumes number of element is LOGICAL_NODE_NUM + 1
#[repr(C)]
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub struct GCDEntry {
    pub wmeta_: SeqLock,
    pub wmeta_idx_: isize,
    pub cn_array_: [CNStatus; LOGICAL_NODE_NUM + 1],
}

pub fn init_gcd_entry(cn: isize, idx: u64) -> GCDEntry {
    let mut array = [null_CNStatus(); LOGICAL_NODE_NUM + 1];

    if idx < (LOGICAL_NODE_NUM + 1) as u64 {
        array[idx as usize] = CNStatus {
            cn_idx_: cn,
            invalidate_: false,
        };
    }

    GCDEntry {
        wmeta_: null_SeqLock(),
        wmeta_idx_: -1,
        cn_array_: array,
    }
}

pub fn init_empty_gcd_entry() -> GCDEntry {
    let array = [null_CNStatus(); LOGICAL_NODE_NUM + 1];

    GCDEntry {
        wmeta_: null_SeqLock(),
        wmeta_idx_: -1,
        cn_array_: array,
    }
}

pub fn init_empty_gcd_entry_with_wmeta_idx(wmeta_idx: isize) -> GCDEntry {
    let array = [null_CNStatus(); LOGICAL_NODE_NUM + 1];

    GCDEntry {
        wmeta_: null_SeqLock(),
        wmeta_idx_: wmeta_idx,
        cn_array_: array,
    }
}

// BlockId definition that toggles based on KEY_SIZE constant
// When KEY_SIZE == 24: matches blockid_file.h (server_id, inode, offset)
// When KEY_SIZE != 24: matches blockid_variable.h (offset + padding)
// This matches the C++ behavior: #if KEY_SIZE == 24 ... #else ...
// The build script (build.rs) automatically sets the key_size_24 cfg when KEY_SIZE == 24

// BlockId definition that matches blockid_file.h (when KEY_SIZE == 24)
#[cfg(key_size_24)]
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct BlockId {
    pub server_id_: u64,
    pub inode_: ino_t,
    pub offset_: off_t,
}

#[cfg(key_size_24)]
impl PartialEq for BlockId {
    fn eq(&self, other: &Self) -> bool {
        self.inode_ == other.inode_ && self.offset_ == other.offset_
    }
}

#[cfg(key_size_24)]
impl Eq for BlockId {}

#[cfg(key_size_24)]
impl Hash for BlockId {
    fn hash<H: Hasher>(&self, state: &mut H) {
        self.inode_.hash(state);
        self.offset_.hash(state);
    }
}

#[cfg(key_size_24)]
pub fn null_BlockId() -> BlockId {
    BlockId {
        server_id_: 0,
        inode_: 0,
        offset_: -1,
    }
}

// BlockId definition that matches blockid_variable.h (when KEY_SIZE != 24)
#[cfg(not(key_size_24))]
#[repr(C, packed)]
#[derive(Clone, Copy, Debug)]
pub struct BlockId {
    pub offset_: u64,
    pub padding_: [u8; KEY_SIZE - 8],
}

#[cfg(not(key_size_24))]
impl PartialEq for BlockId {
    fn eq(&self, other: &Self) -> bool {
        // Copy the values first to avoid unaligned references in packed struct
        let self_offset = self.offset_;
        let other_offset = other.offset_;
        self_offset == other_offset
    }
}

#[cfg(not(key_size_24))]
impl Eq for BlockId {}

#[cfg(not(key_size_24))]
impl Hash for BlockId {
    fn hash<H: Hasher>(&self, state: &mut H) {
        // Copy the value first to avoid unaligned reference in packed struct
        let offset = self.offset_;
        offset.hash(state);
    }
}

#[cfg(not(key_size_24))]
pub fn null_BlockId() -> BlockId {
    BlockId {
        offset_: u64::MAX, // -1 as u64
        padding_: [0; KEY_SIZE - 8],
    }
}

// Compile-time assertion to ensure KEY_SIZE matches the cfg flag
const _: () = {
    #[cfg(key_size_24)]
    const _ASSERT_KEY_SIZE: () = assert!(KEY_SIZE == 24, "key_size_24 cfg requires KEY_SIZE == 24");
    
    #[cfg(not(key_size_24))]
    const _ASSERT_KEY_SIZE: () = assert!(KEY_SIZE != 24, "When KEY_SIZE == 24, build script should set key_size_24 cfg");
};

// Size assertion for variable BlockId (matches C++ static_assert(sizeof(BlockId) == KEY_SIZE))
#[cfg(not(key_size_24))]
const _: () = {
    const _ASSERT_SIZE: () = assert!(
        std::mem::size_of::<BlockId>() == KEY_SIZE,
        "BlockId size must equal KEY_SIZE"
    );
};

// GCDReponse struct
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct GCDReponse {
    pub key: BlockId,
    pub value: GCDEntry,
}

impl PartialEq for GCDReponse {
    fn eq(&self, other: &Self) -> bool {
        self.value.cn_array_[0].cn_idx_ == other.value.cn_array_[0].cn_idx_
    }
}

impl Eq for GCDReponse {}

impl Hash for GCDReponse {
    fn hash<H: Hasher>(&self, state: &mut H) {
        self.value.cn_array_[0].cn_idx_.hash(state);
    }
}

unsafe impl Sync for GCDReponse {}

impl IdxGetter for GCDReponse {
    fn cn_idx(&self) -> isize {
        self.value.cn_array_[0].cn_idx_
    }
}

// OptionGCDReponse struct
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct OptionGCDReponse(pub Option<GCDReponse>);

impl OptionGCDReponse {
    pub fn as_ref(&self) -> Option<&GCDReponse> {
        self.0.as_ref()
    }

    pub fn is_some(&self) -> bool {
        self.0.is_some()
    }

    pub fn unwrap(self) -> GCDReponse {
        self.0.unwrap()
    }
}

impl IdxGetter for OptionGCDReponse {
    fn cn_idx(&self) -> isize {
        self.as_ref().map_or(-1, |r| r.cn_idx())
    }
}

// Debug function for GCDEntry initialization
pub fn init_gcd_entry_debug(key: BlockId, cn: isize, idx: u64) -> GCDEntry {
    let mut array = [null_CNStatus(); LOGICAL_NODE_NUM + 1];

    if idx < (LOGICAL_NODE_NUM + 1) as u64 {
        array[idx as usize] = CNStatus {
            cn_idx_: cn,
            invalidate_: false,
        };
        #[cfg(debug_assertions)]
        {
            let mut lock = stdout().lock();
            #[cfg(key_size_24)]
            {
                write!(
                    lock,
                    "rust_ffi::init_gcd_entry[{}]: \
                            BlockId(server={}, inode={}, offset={}), \
                            array[{}]=@{:p}\n",
                    nix::unistd::gettid(),
                    key.server_id_,
                    key.inode_,
                    key.offset_,
                    idx,
                    cn as *const u32
                )
                .unwrap();
            }
            #[cfg(not(key_size_24))]
            {
                // Copy the value first to avoid unaligned reference in packed struct
                let offset = key.offset_;
                write!(
                    lock,
                    "rust_ffi::init_gcd_entry[{}]: \
                            BlockId(offset={}, keysize={}), \
                            array[{}]=@{:p}\n",
                    nix::unistd::gettid(),
                    offset,
                    KEY_SIZE,
                    idx,
                    cn as *const u32
                )
                .unwrap();
            }
        }
    }

    GCDEntry {
        wmeta_: null_SeqLock(),
        wmeta_idx_: -1,
        cn_array_: array,
    }
}

// NrHashMapFFi struct
/// The node-replicated hashmap uses a std hashmap internally.
#[repr(C)]
#[derive(Default, Clone)]
pub struct NrHashMapFFi {
    pub storage: std::collections::HashMap<BlockId, GCDEntry>,
}

impl NrHashMapFFi {
    pub fn new(cap: usize) -> NrHashMapFFi {
        let storage = std::collections::HashMap::with_capacity(cap);
        NrHashMapFFi { storage }
    }
}

// NrRapper struct
pub struct NrRapper {
    pub nrht: std::sync::Arc<node_replication::nr::NodeReplicated<NrHashMapFFi>>,
    pub thread_handle: Option<std::thread::JoinHandle<()>>,
    pub stop_signal: std::sync::Arc<std::sync::atomic::AtomicBool>,
}

// NrMeta struct
pub struct NrMeta {
    pub nrht: std::sync::Arc<node_replication::nr::NodeReplicated<NrHashMapFFi>>,
    pub ttkn: node_replication::nr::ThreadToken,
}

