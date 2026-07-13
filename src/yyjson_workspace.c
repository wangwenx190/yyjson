#include "yyjson.h"

/*==============================================================================
 * MARK: - Reusable Workspace (Public)
 *============================================================================*/

#if !YYJSON_DISABLE_READER || !YYJSON_DISABLE_WRITER

#if !YYJSON_DISABLE_READER
typedef struct workspace_chunk {
    size_t capacity;
    struct workspace_chunk *next;
} workspace_chunk;
#endif

struct yyjson_workspace {
    yyjson_alc backing;
    size_t cache_limit;
#if !YYJSON_DISABLE_READER
    yyjson_alc reader;
    yyjson_doc *doc;
    workspace_chunk *available;
    size_t read_capacity;
#endif
#if !YYJSON_DISABLE_WRITER
    yyjson_alc writer;
    char *output;
    size_t output_capacity;
#endif
};

#if !YYJSON_DISABLE_READER

static yyjson_inline size_t workspace_reader_limit(const yyjson_workspace *work) {
#if !YYJSON_DISABLE_WRITER
    if (work->output_capacity >= work->cache_limit) return 0;
    return work->cache_limit - work->output_capacity;
#else
    return work->cache_limit;
#endif
}

static void workspace_trim_reader(yyjson_workspace *work, size_t limit) {
    while (work->read_capacity > limit) {
        workspace_chunk *chunk = work->available;
        work->available = chunk->next;
        work->read_capacity -= sizeof(workspace_chunk) + chunk->capacity;
        work->backing.free(work->backing.ctx, chunk);
    }
}

static void *workspace_reader_malloc(void *ctx, size_t size) {
    yyjson_workspace *work = (yyjson_workspace *)ctx;
    workspace_chunk **link = &work->available, *chunk;
    const size_t header = sizeof(workspace_chunk);

    if (yyjson_unlikely(size > SIZE_MAX - header)) return NULL;
    /* The list is sorted by capacity. Grow its largest chunk if needed. */
    while (*link && (*link)->capacity < size && (*link)->next) {
        link = &(*link)->next;
    }
    chunk = *link;
    if (chunk) {
        workspace_chunk *next = chunk->next;
        size_t capacity = chunk->capacity;
        if (capacity < size) {
            chunk = (workspace_chunk *)work->backing.realloc(
                work->backing.ctx, chunk, header + capacity, header + size);
            if (yyjson_unlikely(!chunk)) return NULL;
            chunk->capacity = size;
        }
        *link = next;
        work->read_capacity -= header + capacity;
    } else {
        chunk = (workspace_chunk *)work->backing.malloc(work->backing.ctx,
                                                       header + size);
        if (yyjson_unlikely(!chunk)) return NULL;
        chunk->capacity = size;
    }
    return chunk + 1;
}

static void *workspace_reader_realloc(void *ctx, void *ptr, size_t old_size,
                                     size_t size) {
    yyjson_workspace *work = (yyjson_workspace *)ctx;
    workspace_chunk *chunk = (workspace_chunk *)ptr - 1;
    const size_t header = sizeof(workspace_chunk);
    (void)old_size;

    if (chunk->capacity >= size) return ptr;
    if (yyjson_unlikely(size > SIZE_MAX - header)) return NULL;
    chunk = (workspace_chunk *)work->backing.realloc(
        work->backing.ctx, chunk, header + chunk->capacity, header + size);
    if (yyjson_unlikely(!chunk)) return NULL;
    chunk->capacity = size;
    return chunk + 1;
}

static void workspace_reader_free(void *ctx, void *ptr) {
    yyjson_workspace *work = (yyjson_workspace *)ctx;
    workspace_chunk *chunk = (workspace_chunk *)ptr - 1;
    workspace_chunk **link = &work->available;
    size_t size = sizeof(workspace_chunk) + chunk->capacity;
    size_t budget = workspace_reader_limit(work);

    if (size > budget) {
        work->backing.free(work->backing.ctx, chunk);
        return;
    }
    if (yyjson_unlikely(work->read_capacity > budget - size)) {
        workspace_trim_reader(work, budget - size);
    }
    while (*link && (*link)->capacity < chunk->capacity) link = &(*link)->next;
    chunk->next = *link;
    *link = chunk;
    work->read_capacity += size;
}

static yyjson_inline void workspace_clear_document(yyjson_workspace *work) {
    yyjson_doc *doc = work->doc;
    if (doc) {
        work->doc = NULL;
        if (doc->str_pool) workspace_reader_free(work, doc->str_pool);
        workspace_reader_free(work, doc);
    }
}

#endif /* !YYJSON_DISABLE_READER */

#if !YYJSON_DISABLE_WRITER

/* The immutable writer keeps its output and nesting stack in one buffer. */
static void *workspace_writer_realloc(void *ctx, void *ptr, size_t old_size,
                                     size_t size) {
    yyjson_workspace *work = (yyjson_workspace *)ctx;
    char *out;
    (void)ptr;
    (void)old_size;
    if (work->output_capacity >= size) return work->output;
    if (work->output) {
        out = (char *)work->backing.realloc(work->backing.ctx, work->output,
                                          work->output_capacity, size);
    } else {
        out = (char *)work->backing.malloc(work->backing.ctx, size);
    }
    if (out) {
        work->output = out;
        work->output_capacity = size;
    }
    return out;
}

static void *workspace_writer_malloc(void *ctx, size_t size) {
    return workspace_writer_realloc(ctx, NULL, 0, size);
}

static void workspace_writer_free(void *ctx, void *ptr) {
    yyjson_workspace *work = (yyjson_workspace *)ctx;
    work->backing.free(work->backing.ctx, ptr);
    work->output = NULL;
    work->output_capacity = 0;
}

#endif /* !YYJSON_DISABLE_WRITER */

#if defined(YYJSON_CUSTOM_ALC)
extern const yyjson_alc YYJSON_CUSTOM_ALC;
#elif !YYJSON_FREESTANDING

static void *workspace_malloc(void *ctx, size_t size) {
    (void)ctx;
    return malloc(size);
}

static void *workspace_realloc(void *ctx, void *ptr, size_t old_size, size_t size) {
    (void)ctx;
    (void)old_size;
    return realloc(ptr, size);
}

static void workspace_free(void *ctx, void *ptr) {
    (void)ctx;
    free(ptr);
}

static const yyjson_alc workspace_default_alc = {
    workspace_malloc, workspace_realloc, workspace_free, NULL
};

#endif

yyjson_workspace *yyjson_workspace_new(size_t cache_limit,
                                       const yyjson_alc *alc_ptr) {
    yyjson_alc alc;
    yyjson_workspace *work;
    if (!alc_ptr) {
#if defined(YYJSON_CUSTOM_ALC)
        alc_ptr = &YYJSON_CUSTOM_ALC;
#elif !YYJSON_FREESTANDING
        alc_ptr = &workspace_default_alc;
#else
        return NULL;
#endif
    }
    alc = *alc_ptr;
    if (!alc.malloc || !alc.realloc || !alc.free) return NULL;
    work = (yyjson_workspace *)alc.malloc(alc.ctx, sizeof(yyjson_workspace));
    if (yyjson_unlikely(!work)) return NULL;
    memset(work, 0, sizeof(*work));
    work->backing = alc;
    work->cache_limit = cache_limit;
#if !YYJSON_DISABLE_READER
    work->reader.malloc = workspace_reader_malloc;
    work->reader.realloc = workspace_reader_realloc;
    work->reader.free = workspace_reader_free;
    work->reader.ctx = work;
#endif
#if !YYJSON_DISABLE_WRITER
    work->writer.malloc = workspace_writer_malloc;
    work->writer.realloc = workspace_writer_realloc;
    work->writer.free = workspace_writer_free;
    work->writer.ctx = work;
#endif
    return work;
}

void yyjson_workspace_reset(yyjson_workspace *work) {
    if (yyjson_unlikely(!work)) return;
#if !YYJSON_DISABLE_WRITER
    if (work->output_capacity > work->cache_limit) {
        workspace_writer_free(work, work->output);
    }
#endif
#if !YYJSON_DISABLE_READER
    workspace_clear_document(work);
#endif
}

void yyjson_workspace_free(yyjson_workspace *work) {
    if (yyjson_unlikely(!work)) return;
    work->cache_limit = 0;
    yyjson_workspace_reset(work);
#if !YYJSON_DISABLE_READER
    workspace_trim_reader(work, 0);
#endif
    work->backing.free(work->backing.ctx, work);
}

#if !YYJSON_DISABLE_READER

static yyjson_inline const yyjson_doc *workspace_read(yyjson_workspace *work, char *dat,
                                               size_t len, size_t capacity,
                                               yyjson_read_flag flg,
                                               yyjson_read_err *err) {
    if (yyjson_likely(work)) {
        workspace_clear_document(work);
        if (!(flg & YYJSON_READ_INSITU) ||
            (capacity >= len && capacity - len >= YYJSON_PADDING_SIZE)) {
            work->doc = yyjson_read_opts(dat, len, flg, &work->reader, err);
            return work->doc;
        }
    }
    if (err) {
        err->code = YYJSON_READ_ERROR_INVALID_PARAMETER;
        err->msg = work ? "input buffer has insufficient padding" : "workspace is NULL";
        err->pos = 0;
    }
    return NULL;
}

const yyjson_doc *yyjson_workspace_read(yyjson_workspace *work, const char *dat,
                                        size_t len, yyjson_read_flag flg,
                                        yyjson_read_err *err) {
    return workspace_read(work, yyjson_constcast(char *)dat, len, 0,
                          flg & ~YYJSON_READ_INSITU, err);
}

const yyjson_doc *yyjson_workspace_read_in_situ(yyjson_workspace *work,
                                                char *dat, size_t len,
                                                size_t capacity,
                                                yyjson_read_flag flg,
                                                yyjson_read_err *err) {
    return workspace_read(work, dat, len, capacity, flg | YYJSON_READ_INSITU, err);
}

#endif /* !YYJSON_DISABLE_READER */

#if !YYJSON_DISABLE_WRITER

const char *yyjson_workspace_write(yyjson_workspace *work, const yyjson_val *val,
                                   yyjson_write_flag flg, size_t *len,
                                   yyjson_write_err *err) {
    yyjson_write_err tmp_err;
    size_t out_len;
    char *out;
    if (yyjson_unlikely(!work)) {
        if (err) {
            err->code = YYJSON_WRITE_ERROR_INVALID_PARAMETER;
            err->msg = "workspace is NULL";
        }
        if (len) *len = 0;
        return NULL;
    }
    if (!err) err = &tmp_err;
    if (work->output) {
        out_len = yyjson_val_write_buf(work->output, work->output_capacity,
                                      val, flg, err);
        if (yyjson_likely(out_len != 0)) {
            if (len) *len = out_len;
            return work->output;
        }
        if (err->code != YYJSON_WRITE_ERROR_MEMORY_ALLOCATION) {
            if (len) *len = 0;
            return NULL;
        }
    }
    /* Grow through the existing writer when the retained buffer is too small. */
    out = yyjson_val_write_opts(val, flg, &work->writer, &out_len, err);
#if !YYJSON_DISABLE_READER
    if (yyjson_unlikely(work->read_capacity &&
                 work->read_capacity > workspace_reader_limit(work))) {
        workspace_trim_reader(work, workspace_reader_limit(work));
    }
#endif
    if (len) *len = out_len;
    return out;
}

#endif /* !YYJSON_DISABLE_WRITER */

#endif /* !YYJSON_DISABLE_READER || !YYJSON_DISABLE_WRITER */
