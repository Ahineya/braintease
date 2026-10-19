/* Host scalar acceleration ABI. Values are IEEE bits, never guest pointers. */
#ifndef VM_MATH_H
#define VM_MATH_H
#include <vm_devices.h>
#define VM_DEVICE_MATH 15
#define VM_MATH_CELLS 16
/* C16 stores each uint64 as four consecutive least-significant-first cells. */
typedef struct VmMathRequest {
    unsigned short version, length, operation, reserved;
    unsigned long long a, b, result;
} VmMathRequest;
#define VM_MATH_QUERY 0
#define RIPPLE_TIME_MATH 0x4D41U
#define VM_MATH_F32_ADD 0x101U
#define VM_MATH_F32_SUB 0x102U
#define VM_MATH_F32_MUL 0x103U
#define VM_MATH_F32_DIV 0x104U
#define VM_MATH_F32_FROM_UI 0x105U
#define VM_MATH_F32_FROM_SI 0x106U
#define VM_MATH_F32_FROM_U64 0x107U
#define VM_MATH_F32_FROM_I64 0x108U
#define VM_MATH_F32_TO_UI 0x109U
#define VM_MATH_F32_TO_SI 0x10AU
#define VM_MATH_F32_TO_U64 0x10BU
#define VM_MATH_F32_TO_I64 0x10CU
#define VM_MATH_F32_FROM_OTHER 0x10DU
#define VM_MATH_F32_EQ 0x10EU
#define VM_MATH_F32_NE 0x10FU
#define VM_MATH_F32_LT 0x110U
#define VM_MATH_F32_LE 0x111U
#define VM_MATH_F32_GT 0x112U
#define VM_MATH_F32_GE 0x113U
#define VM_MATH_F64_ADD 0x201U
#define VM_MATH_F64_SUB 0x202U
#define VM_MATH_F64_MUL 0x203U
#define VM_MATH_F64_DIV 0x204U
#define VM_MATH_F64_FROM_UI 0x205U
#define VM_MATH_F64_FROM_SI 0x206U
#define VM_MATH_F64_FROM_U64 0x207U
#define VM_MATH_F64_FROM_I64 0x208U
#define VM_MATH_F64_TO_UI 0x209U
#define VM_MATH_F64_TO_SI 0x20AU
#define VM_MATH_F64_TO_U64 0x20BU
#define VM_MATH_F64_TO_I64 0x20CU
#define VM_MATH_F64_FROM_OTHER 0x20DU
#define VM_MATH_F64_EQ 0x20EU
#define VM_MATH_F64_NE 0x20FU
#define VM_MATH_F64_LT 0x210U
#define VM_MATH_F64_LE 0x211U
#define VM_MATH_F64_GT 0x212U
#define VM_MATH_F64_GE 0x213U
#define VM_MATH_SQRT 0x301U
#define VM_MATH_FLOOR 0x302U
#define VM_MATH_CEIL 0x303U
#define VM_MATH_TRUNC 0x304U
#define VM_MATH_ROUND 0x305U
#define VM_MATH_SIN 0x306U
#define VM_MATH_COS 0x307U
#define VM_MATH_TAN 0x308U
#define VM_MATH_ASIN 0x309U
#define VM_MATH_ACOS 0x30AU
#define VM_MATH_ATAN 0x30BU
#define VM_MATH_ATAN2 0x30CU
#define VM_MATH_SINH 0x30DU
#define VM_MATH_COSH 0x30EU
#define VM_MATH_TANH 0x30FU
#define VM_MATH_ASINH 0x310U
#define VM_MATH_ACOSH 0x311U
#define VM_MATH_ATANH 0x312U
#define VM_MATH_EXP 0x313U
#define VM_MATH_EXPM1 0x314U
#define VM_MATH_LOG 0x315U
#define VM_MATH_LOG1P 0x316U
#define VM_MATH_LOG2 0x317U
#define VM_MATH_LOG10 0x318U
#define VM_MATH_POW 0x319U
#define VM_MATH_CBRT 0x31AU
#define VM_MATH_HYPOT 0x31BU
#define VM_MATH_FABS 0x31CU
#define VM_MATH_FMOD 0x31DU
#define VM_MATH_COPYSIGN 0x31EU
#define VM_MATH_FMIN 0x31FU
#define VM_MATH_FMAX 0x320U
#define VM_MATH_RINT 0x321U

/* One STORE submits a stack-local packet atomically, including in user mode.
 * Reserved becomes the completion status; failure preserves result words.
 * MMU feature bit 2 must be checked before user MMIO access on older hosts. */
#ifdef VM_MATH_STACK
#include <mmio.h>
static unsigned vm_math_stack(volatile VmMathRequest *packet) {
    unsigned sb;
    unsigned long address = (unsigned long)packet;
#ifdef RIPPLE_MMU
    unsigned features;
    __asm__("CSRR %0, 28" : "=r"(features));
    if (!(features & 4U)) return VM_DEVICE_UNSUPPORTED;
#else
    if (mmio_read(MMIO_MATH_STACK) != 1) return VM_DEVICE_UNSUPPORTED;
#endif
    __asm__("ADD %0, SB, R0" : "=r"(sb));
    if ((unsigned)(address >> 16) != sb) return VM_DEVICE_UNSUPPORTED;
    mmio_write(MMIO_MATH_STACK, (unsigned short)address);
    return packet->reserved;
}
#endif

/* Kernel/standalone submission: descriptor addresses must be physical.
 * The caller serializes the shared mailbox (including against IRQ handlers).
 * No outputs change on failure. QUERY returns bit0=f32, bit1=f64, bit2=libm.
 * The legacy 16-bit device capability bitmap is full; probe QUERY instead. */
#ifdef VM_MATH_PHYSICAL
#include <mmio.h>
static unsigned vm_math_submit(volatile unsigned short *packet) {
    unsigned long address = (unsigned long)packet;
    if (mmio_read(MMIO_DEVICE_VERSION) != VM_DEVICE_ABI_VERSION)
        return VM_DEVICE_UNSUPPORTED;
    mmio_write(MMIO_DEVICE_DESC_BANK, (unsigned short)(address >> 16));
    mmio_write(MMIO_DEVICE_DESC_OFF, (unsigned short)address);
    mmio_write(MMIO_DEVICE_COMMAND, VM_DEVICE_MATH);
    return mmio_read(MMIO_DEVICE_STATUS);
}
#endif

/* User libc hook stages the packet through the FUZIX kernel; native Fuse-X
 * implements the same scalar contract as a direct host service. */
#ifdef RIPPLE_MMU
extern unsigned __rcc_float_accel(unsigned, unsigned long long,
                                unsigned long long, unsigned long long *);
#endif
#endif
