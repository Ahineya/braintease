#!/usr/bin/env bash
# Build and run a C/BFM example from c-test/examples with one command.
#
# Usage:
#   ./scripts/run-example                         # list examples
#   ./scripts/run-example rgb565_tetris
#   ./scripts/run-example doom --disk /path/to/DOOM1.WAD
#   ./scripts/run-example --build-only sierpinski
#   ./scripts/run-example -f 2MHz text40_snake
#
# Each runnable example has a sibling .meta.json:
#   description   Shown by --list
#   display       console | tty | text40 | rgb565
#   visual        If true, pass rvm --visual (needed for RGB565 windows)
#   disk          true = IWAD/disk required; a string is a default path
#   runner        c (default) | doom-c | bfm-doom
#   aliases       Extra names accepted on the command line
#   input         Optional rvm -i string (pre-populated stdin)
#   frequency     Optional default rvm --frequency
#   use_runtime   Link crt0 + libruntime (C examples)
#
# RGB565 demos need --visual so rvm can create the window on the main thread
# before the program calls graphics_init. TEXT40 and TTY demos use the
# terminal and do not pass --visual.
#
# Command-line flags override the metadata.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EXAMPLES="$ROOT/c-test/examples"
BUILD="$ROOT/c-test/build"
RUNTIME="$ROOT/runtime"
BANK_SIZE=64000

RCC="$ROOT/target/release/rcc"
RCPP="$ROOT/target/release/rcpp"
RVM="$ROOT/target/release/rvm"
RASM="$ROOT/src/ripple-asm/target/release/rasm"
RLINK="$ROOT/src/ripple-asm/target/release/rlink"

name=""
list_only=0
build_only=0
rebuild_runtime=0
force_visual=0
no_visual=0
disk=""
frequency=""
input=""
input_set=0
extra=()

usage() {
  cat <<EOF
Usage: $(basename "$0") [options] [example-name] [WAD]

Build and run a program from c-test/examples.

  -l, --list            List examples and exit
  --build-only          Compile/assemble/link only; do not run
  --rebuild-runtime     Force a C runtime rebuild before compiling
  --visual              Force rvm --visual (RGB565 window)
  --no-visual           Do not pass --visual
  --disk PATH           Disk image / IWAD for rvm --disk
  -f, --frequency HZ    Virtual CPU speed passed to rvm (e.g. 2MHz, 500KHz, 60Hz)
  --speed HZ            Same as --frequency
  --input TEXT          Pre-populate rvm stdin (-i)
  -h, --help            Show this help

RGB565 demos open a graphics window and pass --visual automatically.
TEXT40/TTY demos use the terminal and do not.

Without -f, rvm runs as fast as the host allows. With -f, it throttles
to that clock (same as rvm --frequency / rvm -f).

  ./scripts/run-example.sh -f 2MHz text40_snake
  ./scripts/run-example.sh --speed 500KHz rgb565_tetris

Doom demos also need an IWAD:
  ./scripts/run-example.sh doom --disk /path/to/DOOM1.WAD
  DOOM_WAD=/path/to/DOOM1.WAD ./scripts/run-example.sh bfm-doom
EOF
}

# --- tiny JSON helpers for our flat example .meta.json files ---------------

json_str() {
  local file="$1" key="$2" content
  content=$(<"$file")
  if [[ $content =~ \"$key\"[[:space:]]*:[[:space:]]*\"([^\"]*)\" ]]; then
    printf '%s' "${BASH_REMATCH[1]}"
  fi
}

json_bool() {
  local file="$1" key="$2" content
  content=$(<"$file")
  if [[ $content =~ \"$key\"[[:space:]]*:[[:space:]]*(true|false) ]]; then
    printf '%s' "${BASH_REMATCH[1]}"
  fi
}

json_aliases() {
  local file="$1" content inner
  content=$(<"$file")
  if [[ $content =~ \"aliases\"[[:space:]]*:[[:space:]]*\[([^]]*)\] ]]; then
    inner="${BASH_REMATCH[1]}"
    inner=${inner//\"/}
    inner=${inner//,/ }
    # shellcheck disable=SC2086
    set -- $inner
    printf '%s' "$*"
  fi
}

example_stem() {
  local meta="$1" base
  base="${meta##*/}"
  printf '%s' "${base%.meta.json}"
}

example_source() {
  local meta="$1" dir stem
  dir="${meta%/*}"
  stem="$(example_stem "$meta")"
  if [[ -f "$dir/$stem.c" ]]; then
    printf '%s' "$dir/$stem.c"
  elif [[ -f "$dir/$stem.bfm" ]]; then
    printf '%s' "$dir/$stem.bfm"
  fi
}

example_names() {
  local meta="$1" stem aliases
  stem="$(example_stem "$meta")"
  aliases="$(json_aliases "$meta")"
  printf '%s' "$stem"
  if [[ -n "$aliases" ]]; then
    printf ' %s' "$aliases"
  fi
}

each_meta() {
  find "$EXAMPLES" -name '*.meta.json' -print | LC_ALL=C sort
}

# --- listing / matching ----------------------------------------------------

display_label() {
  case "$1" in
    console) printf '%s' "Console (stdout)" ;;
    tty)     printf '%s' "TTY (keyboard, no graphics window)" ;;
    text40)  printf '%s' "TEXT40 (terminal 40x25 color — no --visual)" ;;
    rgb565)  printf '%s' "RGB565 (opens a window — rvm --visual)" ;;
    *)       printf '%s' "$1" ;;
  esac
}

print_group() {
  local display="$1"
  local meta stem desc extras aliases disk_flag visual_flag width=0
  local -a metas=()

  while IFS= read -r meta; do
    [[ -n "$(example_source "$meta")" ]] || continue
    [[ "$(json_str "$meta" display)" == "$display" ]] || continue
    metas+=("$meta")
    stem="$(example_stem "$meta")"
    if (( ${#stem} > width )); then
      width=${#stem}
    fi
  done < <(each_meta)

  if [[ ${#metas[@]} -eq 0 ]]; then
    return 0
  fi

  printf '  %s\n' "$(display_label "$display")"
  for meta in "${metas[@]}"; do
    stem="$(example_stem "$meta")"
    desc="$(json_str "$meta" description)"
    extras=""
    aliases="$(json_aliases "$meta")"
    if [[ -n "$aliases" ]]; then
      extras="aka $aliases"
    fi
    disk_flag="$(json_bool "$meta" disk)"
    if [[ "$disk_flag" == "true" || -n "$(json_str "$meta" disk)" ]]; then
      if [[ -n "$extras" ]]; then extras="$extras; "; fi
      extras="${extras}needs --disk WAD"
    fi
    visual_flag="$(json_bool "$meta" visual)"
    if [[ "$visual_flag" == "true" ]]; then
      if [[ -n "$extras" ]]; then extras="$extras; "; fi
      extras="${extras}--visual"
    fi
    if [[ -n "$extras" ]]; then
      printf "    %-${width}s  %s  (%s)\n" "$stem" "$desc" "$extras"
    else
      printf "    %-${width}s  %s\n" "$stem" "$desc"
    fi
  done
  printf '\n'
}

list_examples() {
  echo "Available examples:"
  echo
  print_group console
  print_group tty
  print_group text40
  print_group rgb565
  cat <<EOF
Usage:
  ./scripts/run-example.sh <name>
  ./scripts/run-example.sh doom --disk /path/to/DOOM1.WAD
  ./scripts/run-example.sh -f 2MHz text40_snake
  ./scripts/run-example.sh --build-only rgb565_tetris
EOF
}

name_matches() {
  local query="$1" meta="$2" n
  for n in $(example_names "$meta"); do
    if [[ "$n" == "$query" ]]; then
      return 0
    fi
  done
  return 1
}

name_prefix() {
  local query="$1" meta="$2" n
  for n in $(example_names "$meta"); do
    if [[ "$n" == "$query"* ]]; then
      return 0
    fi
  done
  return 1
}

name_substr() {
  local query="$1" meta="$2" n
  for n in $(example_names "$meta"); do
    if [[ "$n" == *"$query"* ]]; then
      return 0
    fi
  done
  return 1
}

collect_matches() {
  local mode="$1" query="$2" meta src
  MATCHES=()
  while IFS= read -r meta; do
    src="$(example_source "$meta")"
    [[ -n "$src" ]] || continue
    case "$mode" in
      exact)  name_matches "$query" "$meta" || continue ;;
      prefix) name_prefix "$query" "$meta" || continue ;;
      substr) name_substr "$query" "$meta" || continue ;;
    esac
    MATCHES+=("$meta")
  done < <(each_meta)
}

resolve_example() {
  local query="$1"
  query="${query%.c}"
  query="${query%.bfm}"

  collect_matches exact "$query"
  if [[ ${#MATCHES[@]} -eq 1 ]]; then
    RESOLVED="${MATCHES[0]}"
    return 0
  fi
  if [[ ${#MATCHES[@]} -gt 1 ]]; then
    echo "error: '$query' matches multiple examples" >&2
    return 1
  fi

  collect_matches prefix "$query"
  if [[ ${#MATCHES[@]} -eq 1 ]]; then
    RESOLVED="${MATCHES[0]}"
    return 0
  fi

  if [[ ${#MATCHES[@]} -eq 0 ]]; then
    collect_matches substr "$query"
  fi
  if [[ ${#MATCHES[@]} -eq 1 ]]; then
    RESOLVED="${MATCHES[0]}"
    return 0
  fi
  if [[ ${#MATCHES[@]} -gt 1 ]]; then
    echo "error: '$query' is ambiguous. Matches:" >&2
    local meta
    for meta in "${MATCHES[@]}"; do
      echo "  $(example_stem "$meta")" >&2
    done
    return 1
  fi

  echo "error: unknown example '$query'" >&2
  echo "Run $(basename "$0") with no arguments to list examples." >&2
  return 1
}

# --- toolchain -------------------------------------------------------------

require_tool() {
  local path="$1" label="$2"
  if [[ ! -x "$path" ]]; then
    echo "error: missing $label at $path" >&2
    echo "Build the toolchain first, e.g.:" >&2
    echo "  cargo build --release" >&2
    echo "  (cd src/ripple-asm && cargo build --release)" >&2
    exit 1
  fi
}

ensure_runtime() {
  if [[ "$rebuild_runtime" -eq 1 || ! -f "$RUNTIME/crt0.pobj" || ! -f "$RUNTIME/libruntime.par" ]]; then
    echo "Building C runtime (BANK_SIZE=$BANK_SIZE)"
    if [[ "$rebuild_runtime" -eq 1 ]]; then
      make -C "$RUNTIME" clean
    fi
    make -C "$RUNTIME" all crt0.pobj "BANK_SIZE=$BANK_SIZE"
  fi
}

compile_c_example() {
  local source="$1" use_runtime="$2"
  local stem pp asm pobj bin

  require_tool "$RCC" rcc
  require_tool "$RCPP" rcpp
  require_tool "$RASM" rasm
  require_tool "$RLINK" rlink

  if [[ "$use_runtime" == "true" ]]; then
    ensure_runtime
  fi

  mkdir -p "$BUILD"
  stem="$(basename "$source")"
  stem="${stem%.*}"
  pp="$BUILD/$stem.pp.c"
  asm="$BUILD/$stem.asm"
  pobj="$BUILD/$stem.pobj"
  bin="$BUILD/$stem.bin"

  echo "Preprocessing $source"
  "$RCPP" "$source" -o "$pp" -I "$RUNTIME/include" -I "$(dirname "$source")"
  echo "Compiling $pp"
  "$RCC" compile "$pp" -o "$asm" --no-preprocess --bank-size "$BANK_SIZE"
  echo "Assembling $asm"
  "$RASM" assemble "$asm" -o "$pobj" --bank-size "$BANK_SIZE" --max-immediate 65535

  echo "Linking $bin"
  if [[ "$use_runtime" == "true" ]]; then
    "$RLINK" "$RUNTIME/crt0.pobj" "$RUNTIME/libruntime.par" "$pobj" \
      -f binary --bank-size "$BANK_SIZE" -o "$bin"
  else
    "$RLINK" "$RUNTIME/crt0.pobj" "$pobj" \
      -f binary --bank-size "$BANK_SIZE" -o "$bin"
  fi
  echo "Built $bin"
  BINARY="$bin"
}

default_disk() {
  local meta="$1" path
  path="$(json_str "$meta" disk)"
  if [[ -n "$path" ]]; then
    if [[ "$path" != /* ]]; then
      path="$ROOT/$path"
    fi
    printf '%s' "$path"
    return 0
  fi
  if [[ -n "${DOOM_WAD:-}" ]]; then
    printf '%s' "$DOOM_WAD"
    return 0
  fi
  if [[ -f "$EXAMPLES/doom/doom1.wad" ]]; then
    printf '%s' "$EXAMPLES/doom/doom1.wad"
    return 0
  fi
}

disk_needed() {
  local meta="$1"
  [[ "$(json_bool "$meta" disk)" == "true" || -n "$(json_str "$meta" disk)" ]]
}

# --- args ------------------------------------------------------------------

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help)
      usage
      exit 0
      ;;
    -l|--list)
      list_only=1
      shift
      ;;
    --build-only)
      build_only=1
      shift
      ;;
    --rebuild-runtime)
      rebuild_runtime=1
      shift
      ;;
    --visual)
      force_visual=1
      shift
      ;;
    --no-visual)
      no_visual=1
      shift
      ;;
    --disk)
      disk="${2:?--disk requires a path}"
      shift 2
      ;;
    --disk=*)
      disk="${1#--disk=}"
      shift
      ;;
    -f|--frequency|--speed|--cpu-speed)
      frequency="${2:?$1 requires a value (e.g. 2MHz, 500KHz, 60Hz)}"
      shift 2
      ;;
    --frequency=*|--speed=*|--cpu-speed=*)
      frequency="${1#*=}"
      shift
      ;;
    -f*)
      frequency="${1#-f}"
      if [[ -z "$frequency" ]]; then
        echo "error: -f requires a value (e.g. -f 2MHz)" >&2
        exit 1
      fi
      shift
      ;;
    --input)
      input="${2:?--input requires a value}"
      input_set=1
      shift 2
      ;;
    --input=*)
      input="${1#--input=}"
      input_set=1
      shift
      ;;
    --)
      shift
      extra+=("$@")
      break
      ;;
    -*)
      echo "error: unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
    *)
      if [[ -z "$name" ]]; then
        name="$1"
      elif [[ -z "$disk" ]]; then
        disk="$1"
      else
        extra+=("$1")
      fi
      shift
      ;;
  esac
done

cd "$ROOT"

if [[ "$list_only" -eq 1 || -z "$name" || "$name" == "list" ]]; then
  list_examples
  exit 0
fi

RESOLVED=""
resolve_example "$name"
meta="$RESOLVED"
src="$(example_source "$meta")"
if [[ -z "$src" ]]; then
  echo "error: no .c/.bfm source next to $meta" >&2
  exit 1
fi

runner="$(json_str "$meta" runner)"
runner="${runner:-c}"

visual="$(json_bool "$meta" visual)"
display="$(json_str "$meta" display)"
if [[ -z "$visual" && "$display" == "rgb565" ]]; then
  visual=true
fi
if [[ "$force_visual" -eq 1 ]]; then
  visual=true
fi
if [[ "$no_visual" -eq 1 ]]; then
  visual=false
fi

if [[ -z "$frequency" ]]; then
  frequency="$(json_str "$meta" frequency)"
fi
if [[ "$input_set" -eq 0 ]]; then
  input="$(json_str "$meta" input)"
fi

if [[ -z "$disk" ]] && disk_needed "$meta"; then
  disk="$(default_disk "$meta")"
fi

if [[ "$runner" == "doom-c" ]]; then
  helper=("$ROOT/scripts/run-doom-c.sh")
  if [[ "$build_only" -eq 1 ]]; then
    helper+=(--build-only)
  fi
  if [[ -n "$frequency" ]]; then
    helper+=(--frequency "$frequency")
  fi
  if [[ -n "$disk" ]]; then
    helper+=("$disk")
  fi
  if [[ ${#extra[@]} -gt 0 ]]; then
    helper+=("${extra[@]}")
  fi
  exec "${helper[@]}"
fi

if [[ "$runner" == "bfm-doom" ]]; then
  helper=("$ROOT/scripts/run-bfm-doom.sh")
  if [[ "$build_only" -eq 1 ]]; then
    helper+=(--build-only)
  fi
  if [[ -n "$frequency" ]]; then
    helper+=(--frequency "$frequency")
  fi
  if [[ -n "$disk" ]]; then
    helper+=("$disk")
  fi
  if [[ ${#extra[@]} -gt 0 ]]; then
    helper+=("${extra[@]}")
  fi
  exec "${helper[@]}"
fi

if [[ "$runner" != "c" ]]; then
  echo "error: $meta: unknown runner '$runner'" >&2
  exit 1
fi

if disk_needed "$meta" && [[ -z "$disk" && "$build_only" -eq 0 ]]; then
  echo "error: this example needs a disk image / IWAD" >&2
  echo "Pass --disk PATH, a positional WAD path, or set DOOM_WAD" >&2
  exit 1
fi
if [[ -n "$disk" && ! -f "$disk" && "$build_only" -eq 0 ]]; then
  echo "error: disk image not found: $disk" >&2
  exit 1
fi

use_runtime="$(json_bool "$meta" use_runtime)"
use_runtime="${use_runtime:-true}"

BINARY=""
compile_c_example "$src" "$use_runtime"

if [[ "$build_only" -eq 1 ]]; then
  exit 0
fi

require_tool "$RVM" rvm
rvm_args=("$BINARY")
if [[ "$visual" == "true" ]]; then
  rvm_args+=(--visual)
fi
if [[ -n "$disk" ]]; then
  rvm_args+=(--disk "$disk")
fi
if [[ -n "$frequency" ]]; then
  rvm_args+=(--frequency "$frequency")
fi
if [[ -n "$input" ]]; then
  rvm_args+=(-i "$input")
fi
if [[ ${#extra[@]} -gt 0 ]]; then
  rvm_args+=("${extra[@]}")
fi

echo "Running $RVM ${rvm_args[*]}"
exec "$RVM" "${rvm_args[@]}"
