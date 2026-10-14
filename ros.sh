#!/usr/bin/env bash

ROOT=$PWD
RT=$ROOT/runtime
RCC=$ROOT/target/release/rcc
RASM=$ROOT/src/ripple-asm/target/release/rasm
RLINK=$ROOT/src/ripple-asm/target/release/rlink
OUT=$ROOT/build/ros
BANK=64000

$RCC compile c-test/examples/sierpinski.c -o $OUT/sierpinski.asm --bank-size $BANK -I $RT/include
$RASM assemble $OUT/sierpinski.asm -o $OUT/sierpinski.pobj --bank-size $BANK --max-immediate 65535
$RLINK $RT/ros/crt0_ros.pobj $RT/libros.par $OUT/sierpinski.pobj \
  -f rxe --bank-size $BANK -o $OUT/SIERP.RXE

$RCC compile c-test/examples/brainfuck.c -o $OUT/brainfuck.asm --bank-size $BANK -I $RT/include
$RASM assemble $OUT/brainfuck.asm -o $OUT/brainfuck.pobj --bank-size $BANK --max-immediate 65535
$RLINK $RT/ros/crt0_ros.pobj $RT/libros.par \
  $RT/__char_patch.pobj $RT/string.pobj $RT/malloc.pobj $RT/printf.pobj \
  $RT/ctype.pobj $RT/stdlib.pobj $RT/softfloat.pobj \
  $OUT/brainfuck.pobj \
  -f rxe --bank-size $BANK -o $OUT/BF.RXE

node c-test/examples/rOS/mkfat16.mjs -o $OUT/ros-apps.img \
  $OUT/IO.SYS:IO.SYS \
  $OUT/KERNEL.SYS:KERNEL.SYS \
  $OUT/COMMAND.COM:COMMAND.COM \
  $OUT/HELLO.RXE:HELLO.RXE \
  $OUT/SIERP.RXE:SIERP.RXE \
  $OUT/BF.RXE:BF.RXE

scripts/run-ros.sh --disk $OUT/ros-apps.img