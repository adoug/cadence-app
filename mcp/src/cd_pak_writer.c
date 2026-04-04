#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
/* cd_pak_writer.c - Cadence Engine PAK file writer (PA-6)
 *
 * Task 11.2: PAK file packer (writer)
 *
 * Split from cd_pak.c during PA-6 since the writer is only used by
 * build/export tools (mcp build_tools, pak-tool), not the runtime kernel.
 *
 * Binary format (see cd_pak.h for full spec):
 *   [Header 16 bytes] [Data section ...] [TOC entries ...]
 *
 * The header is written with a placeholder toc_offset, then data is
 * appended sequentially. On finish(), the TOC is written and the header
 * is patched with the final toc_offset.
 */

#include "cadence/cd_pak.h"
#include "cadence/cd_memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Header size is always 16 bytes (data starts right after) */
#define CD_PAK_HEADER_SIZE 16

/* ============================================================================
 * Writer
 * ============================================================================ */

cd_result_t cd_pak_writer_init(cd_pak_writer_t* writer, const char* output_path) {
    if (writer == NULL || output_path == NULL) {
        return CD_ERR_NULL;
    }

    memset(writer, 0, sizeof(*writer));
    snprintf(writer->output_path, sizeof(writer->output_path), "%s", output_path);

    /* Start with a reasonable capacity, grow if needed */
    writer->max_entries = 256;
    writer->entries = (cd_pak_entry_t*)cd_mem_calloc_tagged(writer->max_entries,
                                               sizeof(cd_pak_entry_t), CD_MEM_MCP);
    if (writer->entries == NULL) {
        return CD_ERR_ALLOC;
    }

    FILE* f = fopen(output_path, "wb");
    if (f == NULL) {
        cd_mem_free_tagged(writer->entries);
        writer->entries = NULL;
        return CD_ERR_IO;
    }
    writer->file = f;

    /* Write placeholder header -- toc_offset will be patched in finish() */
    cd_pak_header_t header;
    memcpy(header.magic, CD_PAK_MAGIC, 4);
    header.version     = CD_PAK_VERSION;
    header.entry_count = 0;
    header.toc_offset  = 0;

    if (fwrite(&header, sizeof(header), 1, f) != 1) {
        fclose(f);
        writer->file = NULL;
        return CD_ERR_IO;
    }

    writer->data_bytes = 0;
    return CD_OK;
}

/* Grow entries array if needed */
static cd_result_t pak_writer_grow(cd_pak_writer_t* writer) {
    if (writer->entry_count < writer->max_entries) return CD_OK;
    if (writer->max_entries >= CD_PAK_MAX_ENTRIES) return CD_ERR_FULL;

    uint32_t new_max = writer->max_entries * 2;
    if (new_max > CD_PAK_MAX_ENTRIES) new_max = CD_PAK_MAX_ENTRIES;

    cd_pak_entry_t* new_entries = (cd_pak_entry_t*)cd_mem_realloc_tagged(
        writer->entries, new_max * sizeof(cd_pak_entry_t), CD_MEM_MCP);
    if (new_entries == NULL) return CD_ERR_ALLOC;

    writer->entries = new_entries;
    writer->max_entries = new_max;
    return CD_OK;
}

cd_result_t cd_pak_writer_add_file(cd_pak_writer_t* writer,
                                    const char* uri,
                                    const char* filepath) {
    if (writer == NULL || uri == NULL || filepath == NULL) {
        return CD_ERR_NULL;
    }
    if (writer->file == NULL || writer->entries == NULL) {
        return CD_ERR_INVALID;
    }
    cd_result_t grow_res = pak_writer_grow(writer);
    if (grow_res != CD_OK) return grow_res;

    /* Read source file */
    FILE* src = fopen(filepath, "rb");
    if (src == NULL) {
        return CD_ERR_IO;
    }

    fseek(src, 0, SEEK_END);
    long file_size = ftell(src);
    fseek(src, 0, SEEK_SET);

    if (file_size < 0) {
        fclose(src);
        return CD_ERR_IO;
    }

    /* For zero-length files, still create an entry */
    uint8_t* buf = NULL;
    if (file_size > 0) {
        buf = (uint8_t*)cd_mem_alloc_tagged((size_t)file_size, CD_MEM_MCP);
        if (buf == NULL) {
            fclose(src);
            return CD_ERR_ALLOC;
        }
        if (fread(buf, 1, (size_t)file_size, src) != (size_t)file_size) {
            cd_mem_free_tagged(buf);
            fclose(src);
            return CD_ERR_IO;
        }
    }
    fclose(src);

    /* Write data to PAK */
    FILE* out = (FILE*)writer->file;
    cd_pak_entry_t* entry = &writer->entries[writer->entry_count];

    snprintf(entry->uri, sizeof(entry->uri), "%s", uri);
    entry->data_offset = CD_PAK_HEADER_SIZE + writer->data_bytes;
    entry->data_size   = (uint64_t)file_size;
    entry->flags       = 0;

    if (file_size > 0) {
        if (fwrite(buf, 1, (size_t)file_size, out) != (size_t)file_size) {
            cd_mem_free_tagged(buf);
            return CD_ERR_IO;
        }
        cd_mem_free_tagged(buf);
    }

    writer->data_bytes += (uint32_t)file_size;
    writer->entry_count++;
    return CD_OK;
}

cd_result_t cd_pak_writer_add_data(cd_pak_writer_t* writer,
                                    const char* uri,
                                    const void* data,
                                    size_t size) {
    if (writer == NULL || uri == NULL) {
        return CD_ERR_NULL;
    }
    if (data == NULL && size > 0) {
        return CD_ERR_NULL;
    }
    if (writer->file == NULL || writer->entries == NULL) {
        return CD_ERR_INVALID;
    }
    cd_result_t grow_res = pak_writer_grow(writer);
    if (grow_res != CD_OK) return grow_res;

    FILE* out = (FILE*)writer->file;
    cd_pak_entry_t* entry = &writer->entries[writer->entry_count];

    snprintf(entry->uri, sizeof(entry->uri), "%s", uri);
    entry->data_offset = CD_PAK_HEADER_SIZE + writer->data_bytes;
    entry->data_size   = (uint64_t)size;
    entry->flags       = 0;

    if (size > 0) {
        if (fwrite(data, 1, size, out) != size) {
            return CD_ERR_IO;
        }
    }

    writer->data_bytes += (uint32_t)size;
    writer->entry_count++;
    return CD_OK;
}

cd_result_t cd_pak_writer_finish(cd_pak_writer_t* writer) {
    if (writer == NULL) {
        return CD_ERR_NULL;
    }
    if (writer->file == NULL) {
        return CD_ERR_INVALID;
    }

    FILE* f = (FILE*)writer->file;

    /* TOC starts right after all data */
    uint32_t toc_offset = CD_PAK_HEADER_SIZE + writer->data_bytes;

    /* Write TOC entries */
    for (uint32_t i = 0; i < writer->entry_count; i++) {
        const cd_pak_entry_t* entry = &writer->entries[i];

        uint16_t uri_len = (uint16_t)strlen(entry->uri);
        if (fwrite(&uri_len, sizeof(uri_len), 1, f) != 1) goto write_err;
        if (fwrite(entry->uri, 1, uri_len, f) != uri_len) goto write_err;
        if (fwrite(&entry->data_offset, sizeof(entry->data_offset), 1, f) != 1) goto write_err;
        if (fwrite(&entry->data_size, sizeof(entry->data_size), 1, f) != 1) goto write_err;
        if (fwrite(&entry->flags, sizeof(entry->flags), 1, f) != 1) goto write_err;
    }

    /* Patch header with final entry count and TOC offset */
    fseek(f, 0, SEEK_SET);

    cd_pak_header_t header;
    memcpy(header.magic, CD_PAK_MAGIC, 4);
    header.version     = CD_PAK_VERSION;
    header.entry_count = writer->entry_count;
    header.toc_offset  = toc_offset;

    if (fwrite(&header, sizeof(header), 1, f) != 1) goto write_err;

    fclose(f);
    writer->file = NULL;
    cd_mem_free_tagged(writer->entries);
    writer->entries = NULL;
    return CD_OK;

write_err:
    fclose(f);
    writer->file = NULL;
    cd_mem_free_tagged(writer->entries);
    writer->entries = NULL;
    return CD_ERR_IO;
}
