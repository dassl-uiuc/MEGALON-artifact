@0xb40e4c7b17c25dea;

struct FetchBlockRequest {
    inode @0 :UInt64;
    offset @1 :UInt64;
}

struct FetchBlockResponse {
    content @0 :Data;
}

struct WriteBlockRequest {
    inode @0 :UInt64;
    offset @1 :UInt64;
    content @2 :Data;
}

struct WriteBlockResponse {
    written @0 :UInt64;
}

struct NewSharedMemoryRegion {
    serverId @0 :UInt64;
}