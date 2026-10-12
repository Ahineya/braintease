#!/bin/bash

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Paths
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RASM="../src/ripple-asm/target/release/rasm"
RLINK="../src/ripple-asm/target/release/rlink"
VM="cargo run --release --"

# Configuration
BANK_SIZE=4096

# Counters
TOTAL=0
PASSED=0
FAILED=0

echo "Running Ripple VM Tests"
echo "======================"
echo

# Build the VM first
echo "Building VM..."
cargo build --release --quiet || exit 1
mkdir -p tests/bin || exit 1
echo

# Function to run a single test
run_test() {
    local test_name=$1
    local asm_file="tests/asm/${test_name}.asm"
    local bin_file="tests/bin/${test_name}.bin"
    local expected_file="tests/expected/${test_name}.txt"
    local output_file="tests/${test_name}.out"
    
    TOTAL=$((TOTAL + 1))
    
    # Check if test files exist
    if [ ! -f "$asm_file" ]; then
        echo -e "${RED}✗ $test_name: Assembly file not found${NC}"
        FAILED=$((FAILED + 1))
        return
    fi
    
    if [ ! -f "$expected_file" ]; then
        echo -e "${YELLOW}⚠ $test_name: Expected output file not found, skipping${NC}"
        return
    fi
    
    # Assemble
    $RASM assemble "$asm_file" -o "tests/${test_name}.pobj" --bank-size $BANK_SIZE > /dev/null 2>&1
    if [ $? -ne 0 ]; then
        echo -e "${RED}✗ $test_name: Assembly failed${NC}"
        FAILED=$((FAILED + 1))
        return
    fi
    
    # Link
    $RLINK "tests/${test_name}.pobj" -o "$bin_file" -f binary --bank-size $BANK_SIZE > /dev/null 2>&1
    if [ $? -ne 0 ]; then
        echo -e "${RED}✗ $test_name: Linking failed${NC}"
        FAILED=$((FAILED + 1))
        rm -f "tests/${test_name}.pobj"
        return
    fi
    
    # Run VM
    timeout 5 $VM "$bin_file" > "$output_file" 2>/dev/null
    if [ $? -eq 124 ]; then
        echo -e "${RED}✗ $test_name: Timeout${NC}"
        FAILED=$((FAILED + 1))
        rm -f "tests/${test_name}.pobj" "$output_file"
        return
    fi
    
    # Compare output
    if diff -q "$expected_file" "$output_file" > /dev/null; then
        echo -e "${GREEN}✓ $test_name${NC}"
        PASSED=$((PASSED + 1))
    else
        echo -e "${RED}✗ $test_name: Output mismatch${NC}"
        echo "  Expected:"
        cat "$expected_file" | sed 's/^/    /'
        echo "  Got:"
        cat "$output_file" | sed 's/^/    /'
        FAILED=$((FAILED + 1))
    fi
    
    # Clean up
    rm -f "tests/${test_name}.pobj" "$output_file"
}

# Run all tests
for asm_file in tests/asm/*.asm; do
    if [ -f "$asm_file" ]; then
        test_name=$(basename "$asm_file" .asm)
        run_test "$test_name"
    fi
done

# Summary
echo
echo "======================"
echo "Test Results:"
echo -e "  Passed: ${GREEN}$PASSED${NC}"
echo -e "  Failed: ${RED}$FAILED${NC}"
echo -e "  Total:  $TOTAL"

if [ $FAILED -eq 0 ]; then
    echo -e "\n${GREEN}All tests passed!${NC}"
    exit 0
else
    echo -e "\n${RED}Some tests failed.${NC}"
    exit 1
fi
