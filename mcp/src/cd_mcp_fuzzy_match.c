/* cd_mcp_fuzzy_match.c - Simple fuzzy string matching for MCP error messages
 *
 * Implements Levenshtein edit distance using a stack-allocated matrix.
 * No malloc/free — all computation uses local buffers on the stack.
 */

#include "cadence/cd_mcp_fuzzy_match.h"

#include <string.h>
#include <ctype.h>

/* ============================================================================
 * Internal: min of two uint32_t values
 * ============================================================================ */

static uint32_t cd_fuzzy_min2(uint32_t a, uint32_t b) {
    return (a < b) ? a : b;
}

static uint32_t cd_fuzzy_min3(uint32_t a, uint32_t b, uint32_t c) {
    return cd_fuzzy_min2(cd_fuzzy_min2(a, b), c);
}

/* ============================================================================
 * cd_fuzzy_edit_distance
 *
 * Standard Levenshtein distance with a stack-allocated (CD_FUZZY_MAX_LEN+1)^2
 * matrix. This is ~16 KB for the default max of 64, which is fine for stack.
 * ============================================================================ */

uint32_t cd_fuzzy_edit_distance(const char* a, const char* b) {
    if (a == NULL || b == NULL) {
        return UINT32_MAX;
    }

    size_t len_a = strlen(a);
    size_t len_b = strlen(b);

    if (len_a > CD_FUZZY_MAX_LEN || len_b > CD_FUZZY_MAX_LEN) {
        return UINT32_MAX;
    }

    /* Quick exits */
    if (len_a == 0) return (uint32_t)len_b;
    if (len_b == 0) return (uint32_t)len_a;
    if (strcmp(a, b) == 0) return 0;

    /* Use two rows instead of full matrix to save stack space */
    uint32_t prev[CD_FUZZY_MAX_LEN + 1];
    uint32_t curr[CD_FUZZY_MAX_LEN + 1];

    /* Initialize first row */
    for (size_t j = 0; j <= len_b; j++) {
        prev[j] = (uint32_t)j;
    }

    for (size_t i = 1; i <= len_a; i++) {
        curr[0] = (uint32_t)i;

        for (size_t j = 1; j <= len_b; j++) {
            uint32_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            curr[j] = cd_fuzzy_min3(
                prev[j] + 1,       /* deletion */
                curr[j - 1] + 1,   /* insertion */
                prev[j - 1] + cost /* substitution */
            );
        }

        /* Swap rows */
        memcpy(prev, curr, (len_b + 1) * sizeof(uint32_t));
    }

    return prev[len_b];
}

/* ============================================================================
 * cd_fuzzy_edit_distance_ci
 *
 * Case-insensitive edit distance. Makes lowercase copies on the stack.
 * ============================================================================ */

uint32_t cd_fuzzy_edit_distance_ci(const char* a, const char* b) {
    if (a == NULL || b == NULL) {
        return UINT32_MAX;
    }

    size_t len_a = strlen(a);
    size_t len_b = strlen(b);

    if (len_a > CD_FUZZY_MAX_LEN || len_b > CD_FUZZY_MAX_LEN) {
        return UINT32_MAX;
    }

    char lower_a[CD_FUZZY_MAX_LEN + 1];
    char lower_b[CD_FUZZY_MAX_LEN + 1];

    for (size_t i = 0; i <= len_a; i++) {
        lower_a[i] = (char)tolower((unsigned char)a[i]);
    }
    for (size_t i = 0; i <= len_b; i++) {
        lower_b[i] = (char)tolower((unsigned char)b[i]);
    }

    return cd_fuzzy_edit_distance(lower_a, lower_b);
}

/* ============================================================================
 * cd_fuzzy_best_match
 * ============================================================================ */

const char* cd_fuzzy_best_match(const char* input,
                                 const char* const* candidates,
                                 uint32_t candidate_count) {
    if (input == NULL || candidates == NULL || candidate_count == 0) {
        return NULL;
    }

    uint32_t best_dist = UINT32_MAX;
    const char* best = NULL;

    for (uint32_t i = 0; i < candidate_count; i++) {
        if (candidates[i] == NULL) continue;

        uint32_t dist = cd_fuzzy_edit_distance(input, candidates[i]);
        if (dist < best_dist) {
            best_dist = dist;
            best = candidates[i];
        }
    }

    if (best_dist <= CD_FUZZY_MAX_DISTANCE) {
        return best;
    }

    return NULL;
}

/* ============================================================================
 * cd_fuzzy_best_match_ci
 * ============================================================================ */

const char* cd_fuzzy_best_match_ci(const char* input,
                                    const char* const* candidates,
                                    uint32_t candidate_count) {
    if (input == NULL || candidates == NULL || candidate_count == 0) {
        return NULL;
    }

    uint32_t best_dist = UINT32_MAX;
    const char* best = NULL;

    for (uint32_t i = 0; i < candidate_count; i++) {
        if (candidates[i] == NULL) continue;

        uint32_t dist = cd_fuzzy_edit_distance_ci(input, candidates[i]);
        if (dist < best_dist) {
            best_dist = dist;
            best = candidates[i];
        }
    }

    if (best_dist <= CD_FUZZY_MAX_DISTANCE) {
        return best;
    }

    return NULL;
}
