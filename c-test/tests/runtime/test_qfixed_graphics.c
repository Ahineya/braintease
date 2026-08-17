// Q16.16 fixed-point for graphics and game development
#include <stdio.h>
#include "qfixed.h"

void putchar(int c);

// Simple rasterization using Q16.16
typedef struct {
    q16_16_t x, y;
} point_t;

// Rotation matrix using Q16.16
typedef struct {
    q16_16_t m00, m01;
    q16_16_t m10, m11;
} matrix2_t;

// Create rotation matrix
matrix2_t create_rotation(q16_16_t angle) {
    matrix2_t m;
    m.m00 = q_cos(angle);
    m.m01 = q_neg(q_sin(angle));
    m.m10 = q_sin(angle);
    m.m11 = q_cos(angle);
    return m;
}

// Apply transformation
point_t transform_point(point_t p, matrix2_t m) {
    point_t result;
    result.x = q_add(q_mul(m.m00, p.x), q_mul(m.m01, p.y));
    result.y = q_add(q_mul(m.m10, p.x), q_mul(m.m11, p.y));
    return result;
}

// Camera with zoom and pan
typedef struct {
    q16_16_t zoom;
    point_t offset;
} camera_t;

// Transform world coordinates to screen coordinates
point_t world_to_screen(point_t world, camera_t cam) {
    point_t screen;
    screen.x = q_mul(q_sub(world.x, cam.offset.x), cam.zoom);
    screen.y = q_mul(q_sub(world.y, cam.offset.y), cam.zoom);
    return screen;
}

// Simple AABB collision detection
typedef struct {
    point_t min, max;
} aabb_t;

int aabb_intersects(aabb_t a, aabb_t b) {
    return q_le(a.min.x, b.max.x) && q_ge(a.max.x, b.min.x) &&
           q_le(a.min.y, b.max.y) && q_ge(a.max.y, b.min.y);
}

int main() {
    // Test 1: Point rotation
    point_t p = {q_from_int(10), Q16_16_ZERO};
    q16_16_t angle_90 = Q16_16_PI;  // Using PI as approximation for testing
    angle_90.integer = 1;
    angle_90.frac = 0x921F;  // ~π/2
    
    matrix2_t rot = create_rotation(angle_90);
    point_t rotated = transform_point(p, rot);
    
    // After 90-degree rotation, (10,0) should be near (0,10)
    if (q_abs(rotated.x).integer == 0 && q_abs(rotated.y).integer >= 9) {
        putchar('Y');  // Rotation works
    } else {
        putchar('N');
    }
    
    // Test 2: Camera zoom
    camera_t cam;
    cam.zoom = q_from_int(2);  // 2x zoom
    cam.offset.x = q_from_int(50);
    cam.offset.y = q_from_int(50);
    
    point_t world_pos = {q_from_int(60), q_from_int(60)};
    point_t screen_pos = world_to_screen(world_pos, cam);
    
    // (60-50)*2 = 20
    if (screen_pos.x.integer == 20 && screen_pos.y.integer == 20) {
        putchar('Y');  // Camera transform works
    } else {
        putchar('N');
    }
    
    // Test 3: AABB collision
    aabb_t box1 = {
        {q_from_int(0), q_from_int(0)},
        {q_from_int(10), q_from_int(10)}
    };
    
    aabb_t box2 = {
        {q_from_int(5), q_from_int(5)},
        {q_from_int(15), q_from_int(15)}
    };
    
    aabb_t box3 = {
        {q_from_int(20), q_from_int(20)},
        {q_from_int(30), q_from_int(30)}
    };
    
    if (aabb_intersects(box1, box2) && !aabb_intersects(box1, box3)) {
        putchar('Y');  // Collision detection works
    } else {
        putchar('N');
    }
    
    // Test 4: Smooth scaling animation
    q16_16_t scale_start = Q16_16_ONE;
    q16_16_t scale_end = q_from_int(3);
    
    // Calculate scale at 25%, 50%, 75%
    q16_16_t t25 = (q16_16_t){0, 0x4000};  // 0.25
    q16_16_t t50 = Q16_16_HALF;
    q16_16_t t75 = (q16_16_t){0, 0xC000};  // 0.75
    
    q16_16_t scale25 = q_lerp(scale_start, scale_end, t25);
    q16_16_t scale50 = q_lerp(scale_start, scale_end, t50);
    q16_16_t scale75 = q_lerp(scale_start, scale_end, t75);
    
    // Check if scales are monotonically increasing
    if (q_lt(scale25, scale50) && q_lt(scale50, scale75)) {
        putchar('Y');  // Smooth scaling works
    } else {
        putchar('N');
    }
    
    // Test 5: Distance calculation (for proximity checks)
    point_t entity1 = {q_from_int(0), q_from_int(0)};
    point_t entity2 = {q_from_int(3), q_from_int(4)};
    
    q16_16_t dx = q_sub(entity2.x, entity1.x);
    q16_16_t dy = q_sub(entity2.y, entity1.y);
    q16_16_t dist_squared = q_add(q_mul(dx, dx), q_mul(dy, dy));
    q16_16_t dist = q_sqrt(dist_squared);  // Should be 5
    
    if (dist.integer == 5 && dist.frac < 0x1000) {
        putchar('Y');  // Distance calculation works
    } else {
        putchar('N');
    }
    
    // Test 6: Fixed-point precision preservation
    q16_16_t tiny = (q16_16_t){0, 0x0001};  // Smallest representable value
    q16_16_t accumulated = Q16_16_ZERO;
    
    // Add tiny value 65536 times. Nested loops: 65536 does not fit in 16-bit int.
    for (int hi = 0; hi < 256; hi++) {
        for (int lo = 0; lo < 256; lo++) {
            accumulated = q_add(accumulated, tiny);
        }
    }
    
    // Should equal 1.0
    if (accumulated.integer == 1 && accumulated.frac == 0) {
        putchar('Y');  // Precision preserved
    } else {
        putchar('N');
    }
    
    putchar('\n');
    return 0;
}