// String library implementation for Ripple C runtime
// Uses for loops to avoid compiler issues with compound conditions in while loops

// String copy - copies src to dst including null terminator
// Returns dst
char* strcpy(char* dst, char* src) {
    int i;
    for (i = 0; ; i++) {
        dst[i] = src[i];
        if (!src[i]) break;
    }
    return dst;
}

// String copy with length limit
char* strncpy(char* dst, char* src, int n) {
    int i;
    for (i = 0; i < n; i++) {
        if (src[i]) {
            dst[i] = src[i];
        } else {
            // Pad with nulls
            for (; i < n; i++) {
                dst[i] = 0;
            }
            break;
        }
    }
    return dst;
}

// String length
int strlen(char* str) {
    int len;
    for (len = 0; str[len]; len++) {
        // Just counting
    }
    return len;
}

// String compare
int strcmp(char* s1, char* s2) {
    int i;
    for (i = 0; ; i++) {
        if (!s1[i] || !s2[i]) break;
        if (s1[i] != s2[i]) {
            return s1[i] - s2[i];
        }
    }
    return s1[i] - s2[i];
}

// String compare with length limit
int strncmp(char* s1, char* s2, int n) {
    int i;
    for (i = 0; i < n; i++) {
        if (!s1[i] || !s2[i]) {
            return s1[i] - s2[i];
        }
        if (s1[i] != s2[i]) {
            return s1[i] - s2[i];
        }
    }
    return 0;
}

// String concatenate
char* strcat(char* dst, char* src) {
    int dst_len = strlen(dst);
    int i;
    for (i = 0; ; i++) {
        dst[dst_len + i] = src[i];
        if (!src[i]) break;
    }
    return dst;
}

// String concatenate with length limit
char* strncat(char* dst, char* src, int n) {
    int dst_len = strlen(dst);
    int i;
    for (i = 0; i < n; i++) {
        if (!src[i]) break;
        dst[dst_len + i] = src[i];
    }
    dst[dst_len + i] = 0;
    return dst;
}

// Find character in string
char* strchr(char* str, int c) {
    int i;
    for (i = 0; str[i]; i++) {
        if (str[i] == c) {
            return (char*)&str[i];
        }
    }
    if (c == 0) {
        return (char*)&str[i];  // Return pointer to null terminator
    }
    return 0;  // NULL
}

// Find last occurrence of character in string
char* strrchr(char* str, int c) {
    char* last = 0;
    int i;
    for (i = 0; str[i]; i++) {
        if (str[i] == c) {
            last = (char*)&str[i];
        }
    }
    if (c == 0) {
        return (char*)&str[i];  // Return pointer to null terminator
    }
    return last;
}

// Memory copy
void* memcpy(void* dst, void* src, unsigned int n) {
    char* d = (char*)dst;
    char* s = (char*)src;
    unsigned int i;
    for (i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dst;
}

// Memory move (handles overlapping regions)
void* memmove(void* dst, void* src, unsigned int n) {
    char* d = (char*)dst;
    char* s = (char*)src;
    unsigned int i;
    
    if (d < s || d >= s + n) {
        // No overlap or dst before src - copy forward
        for (i = 0; i < n; i++) {
            d[i] = s[i];
        }
    } else {
        // Overlap with dst after src - copy backward
        i = n;
        while (i) {
            i = i - 1;
            d[i] = s[i];
        }
    }
    return dst;
}

// Memory set
void* memset(void* dst, int c, unsigned int n) {
    char* d = (char*)dst;
    unsigned int i;
    for (i = 0; i < n; i++) {
        d[i] = (char)c;
    }
    return dst;
}

// Memory compare
int memcmp(void* s1, void* s2, unsigned int n) {
    char* p1 = (char*)s1;
    char* p2 = (char*)s2;
    unsigned int i;
    for (i = 0; i < n; i++) {
        if (p1[i] != p2[i]) {
            return p1[i] - p2[i];
        }
    }
    return 0;
}