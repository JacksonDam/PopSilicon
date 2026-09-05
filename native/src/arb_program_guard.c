#include "arb_program_guard.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int is_arb_program(const char *source, size_t size)
{
    static const char vertex_header[] = "!!ARBvp1.0";
    static const char fragment_header[] = "!!ARBfp1.0";
    return size >= sizeof(vertex_header) - 1 &&
        (!memcmp(source, vertex_header, sizeof(vertex_header) - 1) ||
         !memcmp(source, fragment_header, sizeof(fragment_header) - 1));
}

static int parse_unary(const char *line, size_t line_size, const char *opcode,
                       const char **destination, size_t *destination_size,
                       const char **operand, size_t *operand_size)
{
    const char *cursor = line;
    const char *end = line + line_size;
    while (cursor < end && (*cursor == ' ' || *cursor == '\t')) ++cursor;
    if ((size_t)(end - cursor) < 4 || memcmp(cursor, opcode, 3) != 0 ||
        (cursor[3] != ' ' && cursor[3] != '\t')) {
        return 0;
    }
    cursor += 4;
    while (cursor < end && (*cursor == ' ' || *cursor == '\t')) ++cursor;
    const char *destination_start = cursor;
    while (cursor < end && *cursor != ',') ++cursor;
    const char *destination_end = cursor;
    while (destination_end > destination_start &&
           (destination_end[-1] == ' ' || destination_end[-1] == '\t')) {
        --destination_end;
    }
    if (cursor == end || destination_end == destination_start) return 0;
    ++cursor;
    while (cursor < end && (*cursor == ' ' || *cursor == '\t')) ++cursor;
    const char *operand_start = cursor;
    while (cursor < end && *cursor != ';') ++cursor;
    const char *operand_end = cursor;
    while (operand_end > operand_start &&
           (operand_end[-1] == ' ' || operand_end[-1] == '\t')) {
        --operand_end;
    }
    if (cursor == end || operand_end == operand_start) return 0;
    *destination = destination_start;
    *destination_size = (size_t)(destination_end - destination_start);
    *operand = operand_start;
    *operand_size = (size_t)(operand_end - operand_start);
    return 1;
}

char *arb_program_guard_undefined_math(const void *raw_source,
                                       size_t source_size,
                                       size_t *output_size,
                                       size_t *rsq_guard_count,
                                       size_t *rcp_guard_count)
{
    const char *source = raw_source;
    static const char parameter[] =
        "PARAM lp32_math_guard = { 1e-20, -2, 1, 0 };\n";
    static const char scratch[] = "TEMP lp32_rcp_scratch;\n";
    size_t rsq_count = 0;
    size_t rcp_count = 0;
    size_t position = 0;
    if (output_size) *output_size = source_size;
    if (rsq_guard_count) *rsq_guard_count = 0;
    if (rcp_guard_count) *rcp_guard_count = 0;
    if (!source || !is_arb_program(source, source_size)) return NULL;

    while (position < source_size) {
        size_t line_end = position;
        while (line_end < source_size && source[line_end] != '\n') ++line_end;
        const char *destination = NULL;
        const char *operand = NULL;
        size_t destination_size = 0;
        size_t operand_size = 0;
        if (parse_unary(source + position, line_end - position, "RSQ",
                        &destination, &destination_size,
                        &operand, &operand_size)) {
            ++rsq_count;
        } else if (parse_unary(source + position, line_end - position, "RCP",
                               &destination, &destination_size,
                               &operand, &operand_size)) {
            ++rcp_count;
        }
        position = line_end < source_size ? line_end + 1 : line_end;
    }
    if (!rsq_count && !rcp_count) return NULL;

    /* Each guarded instruction grows by at most the fixed text below (about
       330 bytes) plus one extra copy of its own operands, which the source
       size already covers once; 512 leaves margin. */
    size_t count = rsq_count + rcp_count;
    if (count > (SIZE_MAX - 2 * source_size - sizeof(parameter) -
                 sizeof(scratch) - 1) / 512) {
        return NULL;
    }
    size_t capacity = 2 * source_size + sizeof(parameter) + sizeof(scratch) +
        count * 512 + 1;
    char *output = malloc(capacity);
    if (!output) return NULL;
    size_t used = 0;

    const char *first_newline = memchr(source, '\n', source_size);
    size_t header_size = first_newline ?
        (size_t)(first_newline - source) + 1 : source_size;
    while (header_size < source_size) {
        size_t option_end = header_size;
        while (option_end < source_size && source[option_end] != '\n') {
            ++option_end;
        }
        const char *option = source + header_size;
        const char *option_limit = source + option_end;
        while (option < option_limit && (*option == ' ' || *option == '\t')) {
            ++option;
        }
        if ((size_t)(option_limit - option) < 7 ||
            memcmp(option, "OPTION ", 7) != 0) {
            break;
        }
        header_size = option_end < source_size ? option_end + 1 : option_end;
    }
    memcpy(output + used, source, header_size);
    used += header_size;
    memcpy(output + used, parameter, sizeof(parameter) - 1);
    used += sizeof(parameter) - 1;
    memcpy(output + used, scratch, sizeof(scratch) - 1);
    used += sizeof(scratch) - 1;
    position = header_size;
    while (position < source_size) {
        size_t line_end = position;
        while (line_end < source_size && source[line_end] != '\n') ++line_end;
        size_t next = line_end < source_size ? line_end + 1 : line_end;
        const char *destination = NULL;
        const char *operand = NULL;
        size_t destination_size = 0;
        size_t operand_size = 0;
        int is_rsq = parse_unary(source + position, line_end - position,
                                 "RSQ", &destination, &destination_size,
                                 &operand, &operand_size);
        int is_rcp = !is_rsq &&
            parse_unary(source + position, line_end - position, "RCP",
                        &destination, &destination_size,
                        &operand, &operand_size);
        if (!is_rsq && !is_rcp) {
            size_t copy_size = next - position;
            if (copy_size >= capacity - used) {
                free(output);
                return NULL;
            }
            memcpy(output + used, source + position, copy_size);
            used += copy_size;
        } else if (is_rsq) {
            /*
             * The clamped operand goes through the scratch temporary; the
             * destination is written exactly once because it may be a
             * write-only result register (the SM2 vertex programs compute
             * RCP/RSQ straight into result.texcoord), which no ARB program
             * may read back.
             */
            int written = snprintf(
                output + used, capacity - used,
                "ABS lp32_rcp_scratch.x, %.*s;\n"
                "MAX lp32_rcp_scratch.x, lp32_rcp_scratch.x, "
                    "lp32_math_guard.x;\n"
                "RSQ %.*s, lp32_rcp_scratch.x;\n",
                (int)operand_size, operand,
                (int)destination_size, destination);
            if (written < 0 || (size_t)written >= capacity - used) {
                free(output);
                return NULL;
            }
            used += (size_t)written;
        } else {
            int written = snprintf(
                output + used, capacity - used,
                "MOV lp32_rcp_scratch.x, %.*s;\n"
                "SLT lp32_rcp_scratch.y, lp32_rcp_scratch.x, "
                    "lp32_math_guard.w;\n"
                "ABS lp32_rcp_scratch.x, lp32_rcp_scratch.x;\n"
                "MAX lp32_rcp_scratch.x, lp32_rcp_scratch.x, "
                    "lp32_math_guard.x;\n"
                "RCP lp32_rcp_scratch.x, lp32_rcp_scratch.x;\n"
                "MAD lp32_rcp_scratch.y, lp32_rcp_scratch.y, "
                    "lp32_math_guard.y, lp32_math_guard.z;\n"
                "MUL %.*s, lp32_rcp_scratch.x, lp32_rcp_scratch.y;\n",
                (int)operand_size, operand,
                (int)destination_size, destination);
            if (written < 0 || (size_t)written >= capacity - used) {
                free(output);
                return NULL;
            }
            used += (size_t)written;
        }
        position = next;
    }
    output[used] = '\0';
    if (output_size) *output_size = used;
    if (rsq_guard_count) *rsq_guard_count = rsq_count;
    if (rcp_guard_count) *rcp_guard_count = rcp_count;
    return output;
}

/*
 * Rewrite SHADOW1D/SHADOW2D/SHADOWRECT texture targets to their plain forms.
 *
 * The engine's Cg-compiled fragment programs sample projected render targets
 * (the water's reflection/refraction texture, the SM2 water path in
 * particular) with "TXP ..., texture[n], SHADOW2D" although the texture is a
 * plain RGBA colour target and the game never enables
 * GL_TEXTURE_COMPARE_MODE.  ARB_fragment_program_shadow leaves that
 * combination undefined; the NVIDIA/ATI drivers the port shipped against fetch
 * the texel as if the target were 2D, Apple's returns black, which paints
 * every such surface black.  Only the target keyword is changed; the OPTION
 * line stays, which is harmless.
 */
char *arb_program_plain_shadow_targets(const void *raw_source,
                                       size_t source_size,
                                       size_t *output_size,
                                       size_t *rewrite_count)
{
    static const char fragment_header[] = "!!ARBfp1.0";
    static const char keyword[] = "SHADOW";
    const char *source = raw_source;
    *output_size = 0;
    *rewrite_count = 0;
    if (!source || source_size < sizeof(fragment_header) - 1 ||
        memcmp(source, fragment_header, sizeof(fragment_header) - 1) != 0) {
        return NULL;
    }
    char *output = malloc(source_size + 1);
    if (!output) return NULL;
    size_t used = 0;
    size_t position = 0;
    while (position < source_size) {
        /* A target follows "texture[n]," so require that context. */
        if (source_size - position > sizeof(keyword) - 1 + 2 &&
            memcmp(source + position, keyword, sizeof(keyword) - 1) == 0 &&
            (!memcmp(source + position + 6, "2D", 2) ||
             !memcmp(source + position + 6, "1D", 2) ||
             !memcmp(source + position + 6, "RECT", 4))) {
            size_t back = used;
            while (back > 0 && (output[back - 1] == ' ' ||
                                output[back - 1] == '\t')) {
                --back;
            }
            if (back > 0 && output[back - 1] == ',') {
                position += sizeof(keyword) - 1;
                ++*rewrite_count;
                continue;
            }
        }
        output[used++] = source[position++];
    }
    output[used] = '\0';
    if (!*rewrite_count) {
        free(output);
        return NULL;
    }
    *output_size = used;
    return output;
}
