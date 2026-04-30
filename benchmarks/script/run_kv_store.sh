# for variant in c3 c3_static fg_map fullco noco; do
sudo ls
# for variant in fg_map; do
for variant in c3 fg_map fullco; do
    cd ..
    mkdir -p logs/$variant
    cp CMakeLists_${variant}.txt CMakeLists.txt
    ./build.sh > logs/${variant}/build_errors.log 2>&1
    cd benchmarks
    # for i in {1..1}
    # for i in 12
    for i in 1 3 6 9 12 18 24 35 48 60
    do
        mkdir -p logs/${variant}/kv_store_${i}
        ../scripts/set_uncore_frequency.sh 800000
        # sudo RACKOBJ_CONFIG=../config/a.yaml /mydata/rackobj/build/benchmarks/kv_store_fly $i a.csv > logs/${variant}/kv_store_${i}/a.csv 2>&1
        sudo RACKOBJ_CONFIG=../config/a.yaml /mydata/rackobj/build/benchmarks/kv_store_fly $i b.csv > logs/${variant}/kv_store_${i}/b.csv 2>&1
        sudo RACKOBJ_CONFIG=../config/a.yaml /mydata/rackobj/build/benchmarks/kv_store_fly $i c.csv > logs/${variant}/kv_store_${i}/c.csv 2>&1
        ../scripts/set_uncore_frequency.sh
    done
    mkdir -p /mydata/kv_store/$variant
    # # mv /mydata/kv_store/a.csv /mydata/kv_store/$variant/a.csv
    mv /mydata/kv_store/b.csv /mydata/kv_store/$variant/b.csv
    mv /mydata/kv_store/c.csv /mydata/kv_store/$variant/c.csv
done

# ../scripts/set_uncore_frequency.sh 800000
# # for i in {1..48}
# # for i in 60
# for i in 1 3 6 9 12 18 24 35 48 60
# do
#     sudo RACKOBJ_CONFIG=../config/a.yaml /mydata/rackobj/build/benchmarks/kv_store_fly $i a.csv
#     sudo RACKOBJ_CONFIG=../config/a.yaml /mydata/rackobj/build/benchmarks/kv_store_fly $i b.csv
#     sudo RACKOBJ_CONFIG=../config/a.yaml /mydata/rackobj/build/benchmarks/kv_store_fly $i c.csv
# done
# # sudo RACKOBJ_CONFIG=../config/a.yaml /mydata/rackobj/build/benchmarks/kv_store 1
# # sudo RACKOBJ_CONFIG=../config/a.yaml /mydata/rackobj/build/benchmarks/kv_store 2
# # sudo RACKOBJ_CONFIG=../config/a.yaml /mydata/rackobj/build/benchmarks/kv_store 3
# # sudo RACKOBJ_CONFIG=../config/a.yaml /mydata/rackobj/build/benchmarks/kv_store 4
# # sudo RACKOBJ_CONFIG=../config/a.yaml /mydata/rackobj/build/benchmarks/kv_store 5