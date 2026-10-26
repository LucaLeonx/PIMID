#!/usr/bin/env bash
# Run every built PIM kernel benchmark with one shared PIMID system config.
set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRIPT_DIR="$REPO_ROOT/benchmarks/pim_kernels"
PIMID_BIN="${PIMID_BIN:-$REPO_ROOT/build/pimid}"
METHOD="${METHOD:-exec}"
TIER="${TIER:-tiny}"
MPI_RANKS="${MPI_RANKS:-4}"
OMP_THREADS="${OMP_NUM_THREADS:-8}"

usage() {
    echo "Usage: TIER=tiny|small|medium|large $0 CONFIG.yaml" >&2
    echo "Optional environment: METHOD, MPI_RANKS, OMP_NUM_THREADS, PIMID_BIN, RUN_ID" >&2
    exit 2
}

[[ $# -eq 1 ]] || usage
CONFIG="$1"
[[ "$CONFIG" = /* ]] || CONFIG="$PWD/$CONFIG"
[[ -f "$CONFIG" ]] || { echo "Configuration not found: $CONFIG" >&2; exit 2; }
[[ -x "$PIMID_BIN" ]] || {
    if command -v "$PIMID_BIN" >/dev/null 2>&1; then
        PIMID_BIN="$(command -v "$PIMID_BIN")"
    else
        echo "PIMID executable not found: $PIMID_BIN (set PIMID_BIN to override)" >&2
        exit 2
    fi
}

case "$TIER" in
    tiny|small|medium|large) ;;
    *) echo "Invalid TIER '$TIER' (expected tiny, small, medium, or large)" >&2; exit 2 ;;
esac

CONFIG_NAME="$(basename "$CONFIG")"
CONFIG_STEM="${CONFIG_NAME%.*}"
RUN_ID="${RUN_ID:-$(date +%Y%m%d_%H%M%S)}"
LOG_DIR="$REPO_ROOT/logs/${CONFIG_STEM}_${TIER}_${RUN_ID}"
mkdir -p "$LOG_DIR"

echo "Building PIM kernel benchmarks..."
if ! make -C "$SCRIPT_DIR" all; then
    echo "Failed to build PIM kernel benchmarks" >&2
    exit 1
fi

# Use the same workload sizes as the per-kernel benchmark YAMLs. The tiny tier
# is a quick all-workload run; select a larger tier for heavier measurements.
workload_args() {
    case "$TIER:$1" in
        tiny:stream_triad|tiny:vector_add|tiny:reduction) WORKLOAD_ARGS=(--size 1024) ;;
        tiny:gemv) WORKLOAD_ARGS=(--size 64) ;;
        tiny:spmv_csr) WORKLOAD_ARGS=(--size 256 --density 1) ;;
        tiny:bfs) WORKLOAD_ARGS=(--vertices 128 --degree 4) ;;
        tiny:histogram) WORKLOAD_ARGS=(--size 4096 --bins 64) ;;
        tiny:stencil_2d) WORKLOAD_ARGS=(--size 32 --iters 3) ;;
        tiny:linked_list|tiny:hash_map) WORKLOAD_ARGS=(--objects 16 --passes 1) ;;

        small:stream_triad|small:vector_add) WORKLOAD_ARGS=(--size 8192) ;;
        small:gemv) WORKLOAD_ARGS=(--size 256) ;;
        small:spmv_csr) WORKLOAD_ARGS=(--size 1024 --density 1) ;;
        small:bfs) WORKLOAD_ARGS=(--vertices 512 --degree 8) ;;
        small:histogram) WORKLOAD_ARGS=(--size 16384 --bins 256) ;;
        small:reduction) WORKLOAD_ARGS=(--size 16384) ;;
        small:stencil_2d) WORKLOAD_ARGS=(--size 64 --iters 5) ;;
        small:linked_list|small:hash_map) WORKLOAD_ARGS=(--objects 128 --passes 1) ;;

        medium:stream_triad|medium:vector_add) WORKLOAD_ARGS=(--size 65536) ;;
        medium:gemv) WORKLOAD_ARGS=(--size 512) ;;
        medium:spmv_csr) WORKLOAD_ARGS=(--size 4096 --density 1) ;;
        medium:bfs) WORKLOAD_ARGS=(--vertices 2048 --degree 8) ;;
        medium:histogram) WORKLOAD_ARGS=(--size 65536 --bins 1024) ;;
        medium:reduction) WORKLOAD_ARGS=(--size 131072) ;;
        medium:stencil_2d) WORKLOAD_ARGS=(--size 128 --iters 10) ;;
        medium:linked_list|medium:hash_map) WORKLOAD_ARGS=(--objects 1024 --passes 2) ;;

        large:stream_triad|large:vector_add) WORKLOAD_ARGS=(--size 524288) ;;
        large:gemv) WORKLOAD_ARGS=(--size 1024) ;;
        large:spmv_csr) WORKLOAD_ARGS=(--size 16384 --density 1) ;;
        large:bfs) WORKLOAD_ARGS=(--vertices 8192 --degree 16) ;;
        large:histogram) WORKLOAD_ARGS=(--size 262144 --bins 4096) ;;
        large:reduction) WORKLOAD_ARGS=(--size 1048576) ;;
        large:stencil_2d) WORKLOAD_ARGS=(--size 256 --iters 20) ;;
        large:linked_list|large:hash_map) WORKLOAD_ARGS=(--objects 16384 --passes 10) ;;
        *) echo "No workload arguments for tier=$TIER kernel=$1" >&2; return 1 ;;
    esac
}

KERNELS=(stream_triad vector_add gemv spmv_csr bfs histogram reduction stencil_2d linked_list hash_map)
PASS=0
FAIL=0
SKIP=0

echo "Running PIM benchmarks (tier=$TIER, config=$CONFIG_NAME)"
echo "Logs: $LOG_DIR"
for kernel in "${KERNELS[@]}"; do
    for variant in serial omp mpi; do
        if [[ "$variant" == serial ]]; then
            binary="$SCRIPT_DIR/$kernel/$kernel"
        else
            binary="$SCRIPT_DIR/$kernel/${kernel}_${variant}"
        fi

        if [[ ! -x "$binary" ]]; then
            SKIP=$((SKIP + 1))
            continue
        fi

        if ! workload_args "$kernel"; then
            exit 2
        fi

        workload="$(basename "$binary")"
        log="$LOG_DIR/${workload}.log"
        cmd=("$PIMID_BIN" --method "$METHOD" --config "$CONFIG")
        case "$variant" in
            serial) cmd+=(--workload-type serial) ;;
            omp)    cmd+=(--workload-type openmp) ;;
            mpi)    cmd+=(--mpi-ranks "$MPI_RANKS") ;;
        esac
        # PIMID takes benchmark arguments after --workload. Do not pass an
        # --output flag here: it is not a PIMID option and would reach the app.
        cmd+=(--workload "$binary" "${WORKLOAD_ARGS[@]}" --no-power)

        echo "RUN  $workload (tier=$TIER) -> $log"
        if [[ "$variant" == omp ]]; then
            if OMP_NUM_THREADS="$OMP_THREADS" "${cmd[@]}" >"$log" 2>&1; then
                echo "PASS $workload"
                PASS=$((PASS + 1))
            else
                status=$?
                echo "FAIL $workload (exit $status; see $log)"
                FAIL=$((FAIL + 1))
            fi
        elif "${cmd[@]}" >"$log" 2>&1; then
            echo "PASS $workload"
            PASS=$((PASS + 1))
        else
            status=$?
            echo "FAIL $workload (exit $status; see $log)"
            FAIL=$((FAIL + 1))
        fi
    done
done

if (( PASS + FAIL == 0 )); then
    echo "No built PIM kernel binaries found under $SCRIPT_DIR" >&2
    exit 1
fi

echo "Finished: $PASS passed, $FAIL failed, $SKIP skipped. Logs: $LOG_DIR"
(( FAIL == 0 ))
