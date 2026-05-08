#!/usr/bin/env bash

# Usage: select_test.sh [-q|-g] [-r]
#   -q : run tests quietly
#   -g : run tests with the gdb stub enabled
#   -r : clean and rebuild before running

if (( $# < 1 || $# > 2 )); then
  echo "Usage: $0 [-q|-g] [-r]"
  echo "  -q   : run tests quietly (no GDB stub)"
  echo "  -g   : attach via GDB stub (skip build)"
  echo "  -r   : force clean & full rebuild"
  exit 1
fi

MODE="$1"
if [[ "$MODE" != "-q" && "$MODE" != "-g" ]]; then
  echo "Usage: $0 [-q|-g] [-r]"
  exit 1
fi

REBUILD=0
if (( $# == 2 )); then
  if [[ "$2" == "-r" ]]; then
    REBUILD=1
  else
    echo "Unknown option: $2"
    echo "Usage: $0 [-q|-g] [-r]"
    exit 1
  fi
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/../activate"

CONFIG_FILE="${SCRIPT_DIR}/.test_config"
if [[ ! -f "$CONFIG_FILE" ]]; then
  echo "Error: .test_config file not found: ${CONFIG_FILE}" >&2
  exit 1
fi

declare -A config_args
declare -A config_result
tests=()

while IFS=':' read -r test args result_dir; do
  test="${test//$'\r'/}"
  args="${args//$'\r'/}"
  result_dir="${result_dir//$'\r'/}"

  test="$(echo "$test" | xargs)"
  args="$(echo "$args" | xargs)"
  result_dir="$(echo "$result_dir" | xargs)"

  [[ -z "$test" || "$test" == \#* ]] && continue
  [[ -z "$args" || -z "$result_dir" ]] && continue

  config_result["$test"]="$result_dir"
  config_args["$test"]="$args"
  tests+=("$test")
done < "$CONFIG_FILE"

if [[ ! -d "${SCRIPT_DIR}/build" ]]; then
  echo "Build directory not found. Building Pintos threads..."
  make -C "${SCRIPT_DIR}" clean all
fi

if (( REBUILD )); then
  echo "Force rebuilding Pintos threads..."
  make -C "${SCRIPT_DIR}" clean all
fi

STATE_FILE="${SCRIPT_DIR}/.test_status"
declare -A status_map

if [[ -f "$STATE_FILE" ]]; then
  while read -r test stat; do
    test="${test//$'\r'/}"
    stat="${stat//$'\r'/}"
    [[ -z "$test" ]] && continue
    status_map["$test"]="$stat"
  done < "$STATE_FILE"
fi

echo "=== Available Pintos Tests ==="
for i in "${!tests[@]}"; do
  idx=$((i + 1))
  test="${tests[i]}"
  stat="${status_map[$test]:-untested}"

  case "$stat" in
    PASS) color="\e[32m" ;;
    FAIL) color="\e[31m" ;;
    *)    color="\e[0m" ;;
  esac

  printf " ${color}%2d) %s\e[0m\n" "$idx" "$test"
done

read -p "Enter test numbers (e.g. '1 3 5' or '2-4'): " input

tokens=()
for tok in ${input//,/ }; do
  if [[ "$tok" =~ ^([0-9]+)-([0-9]+)$ ]]; then
    for ((n=${BASH_REMATCH[1]}; n<=${BASH_REMATCH[2]}; n++)); do
      tokens+=("$n")
    done
  else
    tokens+=("$tok")
  fi
done

declare -A seen
sel_tests=()
for n in "${tokens[@]}"; do
  if [[ "$n" =~ ^[0-9]+$ ]] && (( n >= 1 && n <= ${#tests[@]} )); then
    idx=$((n - 1))
    if [[ -z "${seen[$idx]}" ]]; then
      sel_tests+=("${tests[idx]}")
      seen[$idx]=1
    fi
  else
    echo "Invalid test number: $n" >&2
    exit 1
  fi
done

echo "Selected tests: ${sel_tests[*]}"

passed=()
failed=()

(
  cd "${SCRIPT_DIR}/build" || exit 1

  count=0
  total=${#sel_tests[@]}

  for test in "${sel_tests[@]}"; do
    echo

    args_full="${config_args[$test]}"
    kernel_args="$(echo "${args_full%%--*}" | xargs)"
    run_args="$(echo "${args_full##*--}" | xargs)"
    dir="${config_result[$test]}"
    res="${dir}/${test}.result"

    mkdir -p "${dir}"

    if [[ "$MODE" == "-q" ]]; then
      cmd="pintos ${kernel_args:+${kernel_args}} -- ${run_args} run ${test}"
      echo "Running ${test} in batch mode..."
      echo "\$ ${cmd}"
      echo

      if make -s "${res}" ARGS="${kernel_args:+${kernel_args}} -- ${run_args}"; then
        if grep -q '^PASS' "${res}"; then
          echo "PASS"
          passed+=("$test")
        else
          echo "FAIL"
          failed+=("$test")
        fi
      else
        echo "FAIL"
        failed+=("$test")
      fi
    else
      echo -e "=== Debugging \e[33m${test}\e[0m ($((count + 1))/${total}) ==="
      echo -e "\e[33mStart the \"Pintos Debug\" debugger in VS Code.\e[0m"
      echo " * QEMU will wait for gdb on localhost:1234."
      echo " * Output is also captured to '${dir}/${test}.output'."
      echo

      cmd="pintos --gdb ${kernel_args:+${kernel_args}} -- ${run_args} run ${test}"
      echo "\$ ${cmd}"
      eval "${cmd}" 2>&1 | tee "${dir}/${test}.output"

      repo_root="${SCRIPT_DIR}/.."
      ck="${repo_root}/${dir}/${test}.ck"
      if [[ -f "$ck" ]]; then
        perl -I "${repo_root}" "$ck" "${dir}/${test}" "${dir}/${test}.result"
        if grep -q '^PASS' "${dir}/${test}.result"; then
          echo "=> PASS"
          passed+=("$test")
        else
          echo "=> FAIL"
          failed+=("$test")
        fi
      else
        echo "=> No .ck script, skipping result."
        failed+=("$test")
      fi

      echo "=== ${test} session end ==="
    fi

    ((count++))
    echo -e "\e[33mtest ${count}/${total} finish\e[0m"
  done

  {
    echo
    echo "=== Test Summary ==="
    echo "Passed: ${#passed[@]}"
    for t in "${passed[@]}"; do echo "  - $t"; done
    echo "Failed: ${#failed[@]}"
    for t in "${failed[@]}"; do echo "  - $t"; done
  } >&2

  for t in "${passed[@]}"; do
    status_map["$t"]="PASS"
  done

  for t in "${failed[@]}"; do
    status_map["$t"]="FAIL"
  done

  > "$STATE_FILE"
  for test in "${!status_map[@]}"; do
    echo "$test ${status_map[$test]}"
  done >| "$STATE_FILE"
)
