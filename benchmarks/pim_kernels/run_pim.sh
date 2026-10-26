#!/bin/bash
# Run PIM kernel benchmarks through pimid
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PIMID_BIN="${PIMID_BIN:-pimid}"
TIER="${TIER:-tiny}"
METHOD="${METHOD:-exec}"
PASS=0; FAIL=0; SKIP=0

KERNELS="stream_triad vector_add gemv spmv_csr bfs histogram reduction stencil_2d"
VARIANTS="serial omp"
[ -x "$(command -v mpirun)" ] && VARIANTS="serial omp mpi"

echo "=== PIM Kernel Benchmarks (tier=$TIER) ==="

# Build if needed
make -C "$SCRIPT_DIR" all 2>&1 | tail -1

for kernel in $KERNELS; do
    for variant in $VARIANTS; do
        # Determine binary and config
        if [ "$variant" = "serial" ]; then
            bin="$SCRIPT_DIR/$kernel/$kernel"
            suffix=""
        else
            bin="$SCRIPT_DIR/$kernel/${kernel}_${variant}"
            suffix="_${variant}"
        fi

        # Determine config file
        if [ "$TIER" = "small" ]; then
            cfg="$SCRIPT_DIR/$kernel/${kernel}${suffix}.yaml"
        else
            cfg="$SCRIPT_DIR/$kernel/${kernel}${suffix}_${TIER}.yaml"
        fi

        name="${kernel}${suffix}_${TIER}"

        if [ ! -x "$bin" ]; then
            echo "SKIP $name (binary not built)"
            SKIP=$((SKIP + 1))
            continue
        fi

        if [ ! -f "$cfg" ]; then
            echo "SKIP $name (config not found: $cfg)"
            SKIP=$((SKIP + 1))
            continue
        fi

        echo -n "RUN  $name... "

        # First verify standalone execution
        if ! "$bin" --size 64 2>&1 | grep -q "BENCH_DONE"; then
            echo "FAIL (standalone)"
            FAIL=$((FAIL + 1))
            continue
        fi

        # Run through pimid if available
        if command -v "$PIMID_BIN" &>/dev/null; then
            logfile="$(mktemp /tmp/pimid_pim_XXXXXX.log)"
            if "$PIMID_BIN" --method "$METHOD" --config "$cfg" --no-power >"$logfile" 2>&1; then
                echo "PASS"
                PASS=$((PASS + 1))
                rm -f "$logfile"
            else
                echo "FAIL (pimid; see $logfile)"
                FAIL=$((FAIL + 1))
            fi
        else
            echo "PASS (standalone only)"
            PASS=$((PASS + 1))
        fi
    done
done

# Serial-only C++ data-structure benchmarks. These do not have tier-specific
# YAML files yet, so run them with a shared PIM config and explicit tier sizes.
# Set PIMID_CPP_CONFIG to use another config.
CPP_CONFIG="${PIMID_CPP_CONFIG:-$SCRIPT_DIR/stream_triad/stream_triad_large.yaml}"
for kernel in linked_list hash_map; do
    bin="$SCRIPT_DIR/$kernel/$kernel"
    name="${kernel}_${TIER}"

    if [ ! -x "$bin" ]; then
        echo "SKIP $name (binary not built)"
        SKIP=$((SKIP + 1))
        continue
    fi

    echo -n "RUN  $name... "
    standalone_output="$("$bin" 2>&1)" || standalone_output=""
    if ! printf '%s\n' "$standalone_output" | grep -q "BENCH_DONE"; then
        echo "FAIL (standalone)"
        FAIL=$((FAIL + 1))
        continue
    fi

    if command -v "$PIMID_BIN" &>/dev/null; then
        if [ ! -f "$CPP_CONFIG" ]; then
            echo "FAIL (PIM config not found: $CPP_CONFIG)"
            FAIL=$((FAIL + 1))
            continue
        fi
        case "$TIER" in
            tiny)   cpp_args=(--objects 16 --passes 1) ;;
            small)  cpp_args=(--objects 128 --passes 1) ;;
            medium) cpp_args=(--objects 1024 --passes 2) ;;
            large)  cpp_args=(--objects 16384 --passes 10) ;;
            *) echo "Invalid TIER '$TIER' (expected tiny, small, medium, or large)" >&2; exit 2 ;;
        esac
        logfile="$(mktemp /tmp/pimid_pim_XXXXXX.log)"
        if "$PIMID_BIN" --method "$METHOD" --config "$CPP_CONFIG" \
            --workload "$bin" "${cpp_args[@]}" --no-power >"$logfile" 2>&1; then
            echo "PASS"
            PASS=$((PASS + 1))
            rm -f "$logfile"
        else
            echo "FAIL (pimid; see $logfile)"
            FAIL=$((FAIL + 1))
        fi
    else
        echo "PASS (standalone only)"
        PASS=$((PASS + 1))
    fi
done

echo ""
echo "PIM Kernels: $PASS pass, $FAIL fail, $SKIP skip"
[ "$FAIL" -eq 0 ] || exit 1
