/* Nested guarded inclusion must not consume one OS file descriptor per level. */
#if !defined(PP_INCLUDE_DEPTH_0)
#define PP_INCLUDE_DEPTH_0
#include __FILE__
#elif !defined(PP_INCLUDE_DEPTH_1)
#define PP_INCLUDE_DEPTH_1
#include __FILE__
#elif !defined(PP_INCLUDE_DEPTH_2)
#define PP_INCLUDE_DEPTH_2
#include __FILE__
#elif !defined(PP_INCLUDE_DEPTH_3)
#define PP_INCLUDE_DEPTH_3
#include __FILE__
#elif !defined(PP_INCLUDE_DEPTH_4)
#define PP_INCLUDE_DEPTH_4
#include __FILE__
#elif !defined(PP_INCLUDE_DEPTH_5)
#define PP_INCLUDE_DEPTH_5
#include __FILE__
#elif !defined(PP_INCLUDE_DEPTH_6)
#define PP_INCLUDE_DEPTH_6
#include __FILE__
#elif !defined(PP_INCLUDE_DEPTH_7)
#define PP_INCLUDE_DEPTH_7
#include __FILE__
#elif !defined(PP_INCLUDE_DEPTH_8)
#define PP_INCLUDE_DEPTH_8
#include __FILE__
#elif !defined(PP_INCLUDE_DEPTH_9)
#define PP_INCLUDE_DEPTH_9
#include __FILE__
#elif !defined(PP_INCLUDE_DEPTH_10)
#define PP_INCLUDE_DEPTH_10
#include __FILE__
#elif !defined(PP_INCLUDE_DEPTH_11)
#define PP_INCLUDE_DEPTH_11
#include __FILE__
#elif !defined(PP_INCLUDE_DEPTH_12)
#define PP_INCLUDE_DEPTH_12
#include __FILE__
#elif !defined(PP_INCLUDE_DEPTH_13)
#define PP_INCLUDE_DEPTH_13
#include __FILE__
#elif !defined(PP_INCLUDE_DEPTH_14)
#define PP_INCLUDE_DEPTH_14
#include __FILE__
#elif !defined(PP_INCLUDE_DEPTH_15)
#define PP_INCLUDE_DEPTH_15
#include __FILE__
#elif !defined(PP_INCLUDE_DEPTH_16)
#define PP_INCLUDE_DEPTH_16
#include __FILE__
#elif !defined(PP_INCLUDE_DEPTH_17)
#define PP_INCLUDE_DEPTH_17
#include __FILE__
#elif !defined(PP_INCLUDE_DEPTH_18)
#define PP_INCLUDE_DEPTH_18
#include __FILE__
#elif !defined(PP_INCLUDE_DEPTH_19)
#define PP_INCLUDE_DEPTH_19
#include __FILE__
#elif !defined(PP_INCLUDE_DEPTH_20)
#define PP_INCLUDE_DEPTH_20
#include __FILE__
#elif !defined(PP_INCLUDE_DEPTH_21)
#define PP_INCLUDE_DEPTH_21
#include __FILE__
#elif !defined(PP_INCLUDE_DEPTH_22)
#define PP_INCLUDE_DEPTH_22
#include __FILE__
#elif !defined(PP_INCLUDE_DEPTH_23)
#define PP_INCLUDE_DEPTH_23
#include __FILE__
#elif !defined(PP_INCLUDE_DEPTH_24)
#define PP_INCLUDE_DEPTH_24
#include __FILE__
#elif !defined(PP_INCLUDE_DEPTH_25)
#define PP_INCLUDE_DEPTH_25
#include __FILE__
#elif !defined(PP_INCLUDE_DEPTH_26)
#define PP_INCLUDE_DEPTH_26
#include __FILE__
#elif !defined(PP_INCLUDE_DEPTH_27)
#define PP_INCLUDE_DEPTH_27
#include __FILE__
#elif !defined(PP_INCLUDE_DEPTH_28)
#define PP_INCLUDE_DEPTH_28
#include __FILE__
#elif !defined(PP_INCLUDE_DEPTH_29)
#define PP_INCLUDE_DEPTH_29
#include __FILE__
#elif !defined(PP_INCLUDE_DEPTH_30)
#define PP_INCLUDE_DEPTH_30
#include __FILE__
#elif !defined(PP_INCLUDE_DEPTH_31)
#define PP_INCLUDE_DEPTH_31
#include __FILE__
#endif
#ifndef PP_INCLUDE_DEPTH_MAIN
#define PP_INCLUDE_DEPTH_MAIN
extern int putchar(int);
int main(void) {
#if defined(PP_INCLUDE_DEPTH_31)
    putchar('Y');
#else
    putchar('N');
#endif
    putchar('\n');
    return 0;
}
#endif
