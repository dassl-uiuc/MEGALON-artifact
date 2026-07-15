CC=gcc
CFLAGS=-c -Wall -O3 -DNDEBUG -march=native
LDLIBS=-lstdc++

RUSTDIR=../ffi
LDRUSTLIBS=-L$(RUSTDIR)/target/release -lnr_hashmap -lhwloc -lnuma
LDRUSTLIBSORIG=-L$(RUSTDIR)/target/release -lnr_hashmap_orig -lhwloc -lnuma

BUILDDIR=build

# all: $(BUILDDIR)/nr_hashmap $(BUILDDIR)/array_nr_hashmap
# all: $(BUILDDIR)/array_nr_hashmap
all: $(BUILDDIR)/coherence_nr_hashmap $(BUILDDIR)/array_nr_hashmap $(BUILDDIR)/array_nr_hashmap_scr $(BUILDDIR)/seqlock_nr_hashmap
libs: clean rust_lib rust_lib_orig rust_lib_noco

build_dir:
	mkdir -p $(BUILDDIR)

$(BUILDDIR)/%.o: %.cpp build_dir
	$(CC) -o $@ $< $(CFLAGS)

.PHONY: rust_lib rust_lib_orig clean

rust_lib:
	cd $(RUSTDIR); RUSTFLAGS="-C target-feature=+clflushopt" cargo build --release --features scr --target-dir target/with_scr
	mkdir -p $(RUSTDIR)/target/release
	cp $(RUSTDIR)/target/with_scr/release/libnr_hashmap.a $(RUSTDIR)/target/release/libnr_hashmap.a 

rust_lib_noco:
	cd $(RUSTDIR); RUSTFLAGS="-C target-feature=+clflushopt" cargo build --release --features "scr no-coherence" --target-dir target/no_co
	mkdir -p $(RUSTDIR)/target/release
	cp $(RUSTDIR)/target/no_co/release/libnr_hashmap.a $(RUSTDIR)/target/release/libnr_hashmap_noco.a 

# original rust lib wout non-coherence
rust_lib_orig:
	cd $(RUSTDIR); cargo build --release --target-dir target/no_scr
	mkdir -p $(RUSTDIR)/target/release
	cp $(RUSTDIR)/target/no_scr/release/libnr_hashmap.a $(RUSTDIR)/target/release/libnr_hashmap_orig.a 

rust_lib_debug:
	cd $(RUSTDIR); RUST_BACKTRACE=1 cargo build

$(BUILDDIR)/nr_hashmap: $(BUILDDIR)/nr_hashmap.o rust_lib_orig
	$(CC) -o $@ $< $(LDLIBS) $(LDRUSTLIBSORIG)

$(BUILDDIR)/array_nr_hashmap: $(BUILDDIR)/nr_hashmap_array.o rust_lib_orig
	$(CC) -o $@ $< $(LDLIBS) $(LDRUSTLIBSORIG)

$(BUILDDIR)/array_nr_hashmap_scr: $(BUILDDIR)/nr_hashmap_array.o rust_lib
	$(CC) -o $@ $< $(LDLIBS) $(LDRUSTLIBS)

$(BUILDDIR)/coherence_nr_hashmap: $(BUILDDIR)/nr_hashmap_coherence.o rust_lib_orig
	$(CC) -o $@ $< $(LDLIBS) $(LDRUSTLIBSORIG)

$(BUILDDIR)/seqlock_nr_hashmap: $(BUILDDIR)/nr_hashmap_seqlock.o rust_lib_orig
	$(CC) -o $@ $< $(LDLIBS) $(LDRUSTLIBSORIG)

clean:
	rm -rf $(BUILDDIR) $(RUSTDIR)/target