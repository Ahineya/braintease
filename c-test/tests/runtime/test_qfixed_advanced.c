// Advanced Q16.16 fixed-point tests - practical usage examples
#include <stdio.h>
#include "qfixed.h"

void putchar(int c);

// Simple physics simulation using Q16.16
typedef struct {
    q16_16_t x, y;      // Position
    q16_16_t vx, vy;    // Velocity
} particle_t;

// 2D vector operations
typedef struct {
    q16_16_t x, y;
} vec2_t;

// Vector magnitude (length)
q16_16_t vec2_magnitude(vec2_t v) {
    q16_16_t x2 = q_mul(v.x, v.x);
    q16_16_t y2 = q_mul(v.y, v.y);
    q16_16_t sum = q_add(x2, y2);
    return q_sqrt(sum);
}

// Normalize vector
vec2_t vec2_normalize(vec2_t v) {
    q16_16_t mag = vec2_magnitude(v);
    vec2_t result;
    
    // Avoid division by zero
    if (q_eq(mag, Q16_16_ZERO)) {
        result.x = Q16_16_ZERO;
        result.y = Q16_16_ZERO;
    } else {
        result.x = q_div(v.x, mag);
        result.y = q_div(v.y, mag);
    }
    
    return result;
}

// Linear interpolation for smooth animation
q16_16_t animate_value(q16_16_t start, q16_16_t end, q16_16_t t) {
    return q_lerp(start, end, t);
}

int main() {
    // Test 1: Physics simulation with gravity
    particle_t p;
    p.x = q_from_int(0);
    p.y = q_from_int(100);
    p.vx = q_from_int(5);
    p.vy = q_from_int(0);
    
    q16_16_t gravity = q_from_int(-10);
    q16_16_t dt = (q16_16_t){0, 0x0666};  // ~0.025 (1/40 second)
    
    // Simulate 10 timesteps
    for (int i = 0; i < 10; i++) {
        q16_16_t gdt = q_mul(gravity, dt);
        q16_16_t vy = p.vy;
        p.vy = q_add(vy, gdt);

        q16_16_t vx = p.vx;
        q16_16_t x = p.x;
        p.x = q_add(x, q_mul(vx, dt));

        vy = p.vy;
        q16_16_t y = p.y;
        p.y = q_add(y, q_mul(vy, dt));
    }
    
    // Check if particle has moved right and fallen
    if (q_gt(p.x, Q16_16_ZERO) && q_lt(p.y, q_from_int(100))) {
        putchar('Y');  // Physics works
    } else {
        putchar('N');
    }
    
    // Test 2: Vector normalization
    vec2_t v = {q_from_int(3), q_from_int(4)};
    q16_16_t mag = vec2_magnitude(v);  // Should be 5
    
    if (mag.integer == 5 && mag.frac < 0x1000) {  // Allow small error
        putchar('Y');  // Magnitude calculation works
    } else {
        putchar('N');
    }
    
    vec2_t normalized = vec2_normalize(v);
    q16_16_t norm_mag = vec2_magnitude(normalized);  // Should be ~1
    
    if (norm_mag.integer == 1 && norm_mag.frac < 0x1000) {
        putchar('Y');  // Normalization works
    } else {
        putchar('N');
    }
    
    // Test 3: Interpolation for animation
    q16_16_t start_pos = q_from_int(0);
    q16_16_t end_pos = q_from_int(100);
    
    // Test at t=0.25
    q16_16_t t1 = (q16_16_t){0, 0x4000};  // 0.25
    q16_16_t pos1 = animate_value(start_pos, end_pos, t1);
    
    if (pos1.integer == 25 && pos1.frac < 0x1000) {
        putchar('Y');  // Interpolation at 0.25 works
    } else {
        putchar('N');
    }
    
    // Test at t=0.5
    q16_16_t t2 = Q16_16_HALF;
    q16_16_t pos2 = animate_value(start_pos, end_pos, t2);
    
    if (pos2.integer == 50 && pos2.frac < 0x1000) {
        putchar('Y');  // Interpolation at 0.5 works
    } else {
        putchar('N');
    }
    
    // Test 4: Trigonometry (approximate)
    q16_16_t angle = Q16_16_ZERO;
    q16_16_t sin_val = q_sin(angle);  // sin(0) = 0
    
    if (q_abs(sin_val).integer == 0 && q_abs(sin_val).frac < 0x1000) {
        putchar('Y');  // sin(0) works
    } else {
        putchar('N');
    }
    
    q16_16_t cos_val = q_cos(angle);  // cos(0) = 1
    
    if (cos_val.integer == 1 && cos_val.frac < 0x1000) {
        putchar('Y');  // cos(0) works
    } else {
        putchar('N');
    }
    
    // Test 5: Clamping for bounds checking
    q16_16_t value = q_from_int(150);
    q16_16_t min = q_from_int(0);
    q16_16_t max = q_from_int(100);
    q16_16_t clamped = q_clamp(value, min, max);
    
    if (q_eq(clamped, max)) {
        putchar('Y');  // Clamping works
    } else {
        putchar('N');
    }
    
    putchar('\n');
    return 0;
}