# # for variant in c3 c3_static fg_map fullco noco; do
# for variant in c3 fg_map; do
#     cd ..
#     mkdir -p logs/$variant
#     cp CMakeLists_${variant}.txt CMakeLists.txt
#     ./build.sh > logs/${variant}/build_errors.log 2>&1
#     cd benchmarks
#     # for i in {1..1}
#     # for i in 1 3 6 9 12 18 24 35 48 60
#     for i in 12
#     do
#         ../scripts/set_uncore_frequency.sh 800000
#         # sudo RACKOBJ_CONFIG=../config/a.yaml gdb --args /mydata/rackobj/build/benchmarks/rack_btree_fly $i a.csv
#         # sudo RACKOBJ_CONFIG=../config/a.yaml /mydata/rackobj/build/benchmarks/rack_btree_fly $i a.csv
#         sudo RACKOBJ_CONFIG=../config/a.yaml /mydata/rackobj/build/benchmarks/rack_btree_fly $i b.csv
#         sudo RACKOBJ_CONFIG=../config/a.yaml /mydata/rackobj/build/benchmarks/rack_btree_fly $i c.csv
#         ../scripts/set_uncore_frequency.sh
#     done
#     mkdir -p /mydata/btree/$variant
#     # mv /mydata/btree/a.csv /mydata/btree/$variant/a.csv
#     mv /mydata/btree/b.csv /mydata/btree/$variant/b.csv
#     mv /mydata/btree/c.csv /mydata/btree/$variant/c.csv
# done

sudo ls
for variant in c3; do
# for variant in c3 fg_map fullco; do
    cd ..
    mkdir -p logs/$variant
    # cp CMakeLists_${variant}.txt CMakeLists.txt
    # ./build.sh > logs/${variant}/build_errors.log 2>&1
    cd benchmarks


    # Start 4k-read test in background
    ./4k-read-test/4k-read-rand-multi -1 0 21 2 -1 > /dev/null 2>&1 &
    BACKGROUND_PID=$!
    
    # for i in {1..1}
    for i in 18
    # for i in 1 3 6 9 12 18 24 35 48 60
    do
        mkdir -p logs/${variant}/btree_${i}
        ../scripts/set_uncore_frequency.sh 800000
        # sudo RACKOBJ_CONFIG=../config/a.yaml /mydata/rackobj/build/benchmarks/rack_btree_fly $i a.csv > logs/${variant}/btree_${i}/a.csv 2>&1
        sudo RACKOBJ_CONFIG=../config/a.yaml /mydata/jiyu/rackobj/build/benchmarks/rack_btree_fly $i b.csv
        # sudo RACKOBJ_CONFIG=../config/a.yaml /mydata/rackobj/build/benchmarks/rack_btree_fly $i c.csv > logs/${variant}/btree_${i}/c.csv 2>&1
        ../scripts/set_uncore_frequency.sh
    done

    # Kill the background process
    kill $BACKGROUND_PID

    mkdir -p /mydata/btree/$variant
    # mv /mydata/btree/a.csv /mydata/btree/$variant/a.csv
    # mv /mydata/btree/b.csv /mydata/btree/$variant/b.csv
    # mv /mydata/btree/c.csv /mydata/btree/$variant/c.csv
done