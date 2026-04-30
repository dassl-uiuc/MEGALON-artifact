CXX := g++                      # Use g++ for C++ compilation
CXXFLAGS := -g -O3 -pthread     # Optimization and threading
LDLIBS := -lnuma                # Libraries to link

BUILDDIR=build

SRCS := test.cc seqcount.cc
OBJS := $(SRCS:.cc=.o)
OBJS := $(addprefix $(BUILDDIR)/, $(OBJS))
TARGET := $(BUILDDIR)/test_seqlock

all: $(TARGET)

build_dir:
	mkdir -p $(BUILDDIR)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

$(BUILDDIR)/%.o: %.cc build_dir
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILDDIR)

.PHONY: all clean