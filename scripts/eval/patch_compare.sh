#!/usr/bin/env bash
# One-command before/after comparison of the coherence patch for one paper experiment.
#
# For every (system, phase) it checks out the matching commit, runs that commit's own evaluation script
# (which builds and runs every data point), collects throughput / average latency into one CSV and plots
# before vs after in the style of the paper's figure.
#
#   ./scripts/eval/patch_compare.sh [-e eval2|eval3|eval4] [-s "<systems>"] [-p "before after"] [-o <out_dir>] [-c]
#
#   -e  experiment (default eval2):
#         eval2  Section 6.2, Figure 5      read-only, dataset size      megalon hcmeta hcmeta-local  ~3 h
#         eval3  Section 6.3, Figure 6      5%/50% writes, dataset size  megalon hcmeta               ~4 h
#         eval4  Section 6.4, Figure 7b(ii) 50% writes, SCR size         megalon hcmeta               ~2 h
#   -s  systems to run (default: all systems of the experiment)
#   -c  collect only: skip checkouts and runs, re-collect the results currently in ${RACKOBJ_RESULT_DIR}<eval>/
#       as the given phase(s) and re-plot
#
# Requirements: a clean working tree (untracked files are fine), RACKOBJ_RESULT_DIR set, sudo, python3 with
# matplotlib. HCMeta pins 8 GB SCR + 36 GB NCR on NUMA node 0 and 36 GB on each other node, so check
# `numactl -H` first. Output: logs/patch-compare/<eval>/{results.csv,<eval>_before_after.png}.
#
# Commits used (override with environment variables):
#   MEGALON_BEFORE=3be2fa7  MEGALON_AFTER=main  HCMETA_BEFORE=7566c76  HCMETA_AFTER=hcmeta

set -u

EVAL=eval2
SYSTEMS=""
PHASES="before after"
OUT_DIR=""
COLLECT_ONLY=0
while getopts "e:s:p:o:ch" opt; do
    case $opt in
        e) EVAL=$OPTARG ;;
        s) SYSTEMS=$OPTARG ;;
        p) PHASES=$OPTARG ;;
        o) OUT_DIR=$OPTARG ;;
        c) COLLECT_ONLY=1 ;;
        *) sed -n '2,24p' "$0"; exit 0 ;;
    esac
done

MEGALON_BEFORE=${MEGALON_BEFORE:-3be2fa7}
MEGALON_AFTER=${MEGALON_AFTER:-main}
HCMETA_BEFORE=${HCMETA_BEFORE:-7566c76}
HCMETA_AFTER=${HCMETA_AFTER:-hcmeta}
SIZES=(2400000 4800000 7200000 9600000 12000000 18000000 24000000)

# data points of each experiment, as "w_ratio zipf num_obj scr_mb" (must match the eval scripts)
POINTS=()
case $EVAL in
    eval2) SYSTEMS=${SYSTEMS:-"megalon hcmeta hcmeta-local"}
           for n in "${SIZES[@]}"; do POINTS+=("0.0 0.99 $n 200"); done ;;
    eval3) SYSTEMS=${SYSTEMS:-"megalon hcmeta"}
           for w in 0.05 0.5; do for n in "${SIZES[@]}"; do POINTS+=("$w 0.99 $n 200"); done; done ;;
    eval4) SYSTEMS=${SYSTEMS:-"megalon hcmeta"}
           for scr in 32 64 128 256 512 1024 2048; do POINTS+=("0.5 0.7 18000000 $scr"); done ;;
    *) echo "Unknown experiment: $EVAL (eval2, eval3 or eval4)"; exit 1 ;;
esac

REPO="${PATCH_CMP_REPO:-$(git -C "$(dirname "$0")" rev-parse --show-toplevel)}"
cd "$REPO" || exit 1
[ -n "${RACKOBJ_RESULT_DIR:-}" ] || { echo "Error: RACKOBJ_RESULT_DIR is not set."; exit 1; }
OUT_DIR=${OUT_DIR:-$REPO/logs/patch-compare/$EVAL}
mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"

# This script lives on the patched branch and the checkouts below replace it, so run from a copy.
if [ -z "${PATCH_CMP_COPY:-}" ]; then
    tmp="$(mktemp -d)"
    cp "$0" "$tmp/patch_compare.sh"
    cp "$REPO/scripts/eval/plot_patch_compare.py" "$tmp/"
    flags=(-e "$EVAL" -s "$SYSTEMS" -p "$PHASES" -o "$OUT_DIR"); [ "$COLLECT_ONLY" = 1 ] && flags+=(-c)
    PATCH_CMP_COPY=$tmp PATCH_CMP_REPO=$REPO exec bash "$tmp/patch_compare.sh" "${flags[@]}"
fi

ORIG_REF="$(git symbolic-ref -q --short HEAD || git rev-parse HEAD)"
restore() { git checkout -q -- . 2>/dev/null; git checkout -q "$ORIG_REF" && echo "Restored checkout to $ORIG_REF"; }
if [ "$COLLECT_ONLY" = 0 ]; then
    # the runs check out other commits (submodule working-tree changes, e.g. the hostrpc patch, are fine)
    if ! git diff --quiet --ignore-submodules || ! git diff --cached --quiet --ignore-submodules; then
        echo "Error: the working tree has uncommitted changes to tracked files; commit or stash them first."
        exit 1
    fi
    trap restore EXIT
    sudo -v || exit 1
    echo "NUMA free memory:"; numactl -H | grep free
fi

CSV="$OUT_DIR/results.csv"
[ -f "$CSV" ] || echo "eval,system,phase,w_ratio,zipf,num_obj,scr_mb,avg_latency_us,throughput_mops" > "$CSV"

for system in $SYSTEMS; do
    case $system in
        megalon)      label=MEGALON;      script=scripts/eval/$EVAL.sh; variant=megalon;          before=$MEGALON_BEFORE; after=$MEGALON_AFTER ;;
        hcmeta)       label=HCMeta;       script=scripts/eval/$EVAL.sh; variant=hcmeta;           before=$HCMETA_BEFORE;  after=$HCMETA_AFTER ;;
        hcmeta-local) label=HCMeta-local; script=scripts/eval/$EVAL-datamove.sh; variant=hcmeta-data-move; before=$HCMETA_BEFORE; after=$HCMETA_AFTER ;;
        *) echo "Unknown system: $system"; exit 1 ;;
    esac
    for phase in $PHASES; do
        ref=$before; [ "$phase" = after ] && ref=$after
        echo "=== $EVAL $label ($phase): $ref"
        if [ "$COLLECT_ONLY" = 0 ]; then
            git checkout -q -- . && git checkout -q --detach "$ref" || exit 1
            # the commit's own eval script builds and runs every data point
            (bash "$script") || echo "warning: $script exited non-zero; checking its results"
            git checkout -q -- .   # the eval scripts edit config/*.yaml and CMakeLists.txt in place
        fi

        res_root="${RACKOBJ_RESULT_DIR}$EVAL/$variant"
        keep="$OUT_DIR/raw/$phase-$system"
        if [ "$COLLECT_ONLY" = 0 ]; then
            rm -rf "$keep"; mkdir -p "$keep"
            cp -r "logs/$EVAL/$variant" "$keep/logs" 2>/dev/null
        fi
        sed -i "/^$EVAL,$label,$phase,/d" "$CSV"
        for point in "${POINTS[@]}"; do
            read -r w z n scr <<< "$point"
            # result directory naming of the main and hcmeta eval scripts
            if [ "$variant" = megalon ]; then d="$res_root/zipfian-$w-$n-$z-${scr}MB"; else d="$res_root/zipfian-$z-$w-$n-${scr}MB"; fi
            [ -d "$d" ] || { echo "Error: missing results $d"; exit 1; }
            # avg_lat.py rwtf prints "[threads,]avg_ns,std_ns,,,ops_per_s"
            stats=$(cd benchmarks/script && python3 avg_lat.py "$d" rwtf | tail -1)
            echo "$stats" | awk -F, -v e="$EVAL" -v s="$label" -v p="$phase" -v w="$w" -v z="$z" -v n="$n" -v m="$scr" \
                '{ printf "%s,%s,%s,%s,%s,%s,%s,%.3f,%.4f\n", e, s, p, w, z, n, m, $(NF-4) / 1000, $NF / 1e6 }' >> "$CSV"
            echo "  w=$w zipf=$z objects=$n scr=${scr}MB: $(tail -1 "$CSV" | cut -d, -f9) Mops/s"
            # keep the raw latency files (several GB per sweep) by moving them out of the way of the next phase
            [ "$COLLECT_ONLY" = 0 ] && mv "$d" "$keep/"
        done
    done
done

python3 "$PATCH_CMP_COPY/plot_patch_compare.py" "$CSV" "$EVAL" "$OUT_DIR/${EVAL}_before_after.png"
echo "Results: $CSV"
echo "Plot:    $OUT_DIR/${EVAL}_before_after.png"
