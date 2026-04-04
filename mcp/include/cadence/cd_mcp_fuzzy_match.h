/* cd_mcp_fuzzy_match.h - Simple fuzzy string matching for MCP error messages
 *
 * Provides Levenshtein edit distance and "did you mean?" suggestion helpers.
 * Used by MCP tool handlers to suggest corrections for misspelled names.
 *
 * All functions use stack-allocated buffers — no malloc needed.
 */
#ifndef CD_MCP_FUZZY_MATCH_H
#define CD_MCP_FUZZY_MATCH_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Maximum string length supported for edit distance computation.
 * Strings longer than this are compared with simple prefix matching.
 */
#define CD_FUZZY_MAX_LEN 64

/**
 * Maximum edit distance to consider a match as a "did you mean?" suggestion.
 */
#define CD_FUZZY_MAX_DISTANCE 2

/**
 * Compute the Levenshtein edit distance between two strings.
 *
 * Uses a stack-allocated matrix bounded by CD_FUZZY_MAX_LEN.
 * If either string exceeds CD_FUZZY_MAX_LEN, returns UINT32_MAX.
 *
 * @param a  First string (null-terminated).
 * @param b  Second string (null-terminated).
 * @return   Edit distance, or UINT32_MAX if strings are too long.
 */
uint32_t cd_fuzzy_edit_distance(const char* a, const char* b);

/**
 * Find the best match for `input` among a list of `candidates`.
 *
 * Returns the candidate with the lowest edit distance, if that distance
 * is <= CD_FUZZY_MAX_DISTANCE. Returns NULL if no candidate is close enough.
 *
 * @param input            The misspelled input string.
 * @param candidates       Array of candidate strings (null-terminated).
 * @param candidate_count  Number of candidates.
 * @return                 Pointer to the best match (from candidates array),
 *                         or NULL if no match is close enough.
 */
const char* cd_fuzzy_best_match(const char* input,
                                 const char* const* candidates,
                                 uint32_t candidate_count);

/**
 * Case-insensitive version of cd_fuzzy_edit_distance.
 *
 * Converts both strings to lowercase before comparison.
 *
 * @param a  First string.
 * @param b  Second string.
 * @return   Edit distance (case-insensitive).
 */
uint32_t cd_fuzzy_edit_distance_ci(const char* a, const char* b);

/**
 * Case-insensitive version of cd_fuzzy_best_match.
 */
const char* cd_fuzzy_best_match_ci(const char* input,
                                    const char* const* candidates,
                                    uint32_t candidate_count);

#ifdef __cplusplus
}
#endif

#endif /* CD_MCP_FUZZY_MATCH_H */
