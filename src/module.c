/* These must also precede stdlib in any force-included allocation shim. */
#ifndef _WIN32
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "aphelion/module.h"
#include "aphelion/parser.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#define MODULE_LIMIT 256u
#define MODULE_DEPTH_LIMIT 64u
#define MODULE_BYTE_LIMIT (64u * 1024u * 1024u)

typedef struct ModuleNode {
    AstProgram *program;
    unsigned state; /* 1 visiting, 2 complete */
} ModuleNode;

typedef struct ModuleLoader {
    AstProgram *result;
    Diagnostics *diagnostics;
    char *root;
    size_t bytes;
    ModuleNode nodes[MODULE_LIMIT];
} ModuleLoader;

static void module_error(ModuleLoader *loader, SourceLocation location,
                         const char *message) {
    loader->result->had_parse_error = true;
    if (loader->diagnostics != NULL) {
        (void)diagnostics_add(loader->diagnostics, location, message);
    }
}

static char *module_copy(const char *text, size_t length) {
    char *copy = (char *)malloc(length + 1u);
    if (copy != NULL) {
        memcpy(copy, text, length);
        copy[length] = '\0';
    }
    return copy;
}

static void module_path_error(ModuleLoader *loader, SourceLocation location,
                              const char *message, const char *path) {
    char detail[768];
    const char *path_for_message = path != NULL ? path : "<path>";
    int prefix_length;
    size_t used;
#ifdef _WIN32
    char normalized_path[sizeof(detail)];
    size_t i;
    if (path != NULL) {
        for (i = 0u; i + 1u < sizeof(normalized_path) && path[i] != '\0'; ++i)
            normalized_path[i] = path[i] == '\\' ? '/' : path[i];
        normalized_path[i] = '\0';
        path_for_message = normalized_path;
    }
#endif
    prefix_length = snprintf(detail, sizeof(detail), "%s: '", message);
    if (prefix_length < 0) {
        module_error(loader, location, "module path diagnostic formatting failed");
        return;
    }
    used = (size_t)prefix_length < sizeof(detail) ? (size_t)prefix_length : sizeof(detail) - 1u;
    while (path_for_message[0] != '\0' && used + 2u < sizeof(detail)) {
        detail[used++] = path_for_message[0];
        ++path_for_message;
    }
    if (used + 1u < sizeof(detail)) detail[used++] = '\'';
    detail[used] = '\0';
    module_error(loader, location, detail);
}

#ifdef _WIN32
static void normalize_display_path(char *path) {
    if (path == NULL) return;
    for (; *path != '\0'; ++path) {
        if (*path == '\\') *path = '/';
    }
}

static wchar_t *native_path(const char *path) {
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    wchar_t *wide;
    if (count <= 0) return NULL;
    wide = (wchar_t *)malloc((size_t)count * sizeof(*wide));
    if (wide == NULL) return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1,
                            wide, count) == 0) {
        free(wide);
        return NULL;
    }
    return wide;
}
#endif

/* Canonical names are allocated by our allocator, including under the fault
 * injector. realpath(NULL) would allocate outside its ownership ledger. */
static char *canonical_file(ModuleLoader *loader, const char *path,
                             SourceLocation location) {
    char *canonical = NULL;
#ifdef _WIN32
    wchar_t *wide = native_path(path);
    wchar_t *resolved = NULL;
    HANDLE handle;
    BY_HANDLE_FILE_INFORMATION info;
    DWORD length;
    int bytes;
    if (wide == NULL) {
        module_error(loader, location, "out of memory converting module filename");
        return NULL;
    }
    handle = CreateFileW(wide, FILE_READ_ATTRIBUTES,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(wide);
    if (handle == INVALID_HANDLE_VALUE) {
        module_path_error(loader, location, "cannot open module source", path);
        return NULL;
    }
    if (GetFileType(handle) != FILE_TYPE_DISK ||
        !GetFileInformationByHandle(handle, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u) {
        module_error(loader, location, "module source must be a regular file");
        (void)CloseHandle(handle);
        return NULL;
    }
    length = GetFinalPathNameByHandleW(handle, NULL, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (length == 0u || length > 32767u) {
        module_error(loader, location, "cannot canonicalize module source");
        (void)CloseHandle(handle);
        return NULL;
    }
    resolved = (wchar_t *)malloc(((size_t)length + 1u) * sizeof(*resolved));
    if (resolved == NULL) {
        module_error(loader, location, "out of memory canonicalizing module source");
        (void)CloseHandle(handle);
        return NULL;
    }
    {
        DWORD written = GetFinalPathNameByHandleW(handle, resolved, length + 1u,
                                                 FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        (void)CloseHandle(handle);
        if (written == 0u || written > length) {
            module_error(loader, location, "cannot canonicalize module source");
            free(resolved);
            return NULL;
        }
    }
    bytes = WideCharToMultiByte(CP_UTF8, 0, resolved, -1, NULL, 0, NULL, NULL);
    if (bytes > 0) canonical = (char *)malloc((size_t)bytes);
    if (canonical != NULL &&
        WideCharToMultiByte(CP_UTF8, 0, resolved, -1, canonical, bytes, NULL, NULL) == 0) {
        free(canonical);
        canonical = NULL;
    }
    free(resolved);
    if (canonical != NULL) {
        if (strncmp(canonical, "\\\\?\\UNC\\", 8u) == 0) {
            /* Present normal canonical UNC names to source tooling. */
            memmove(canonical + 2u, canonical + 8u, strlen(canonical + 8u) + 1u);
        } else if (strncmp(canonical, "\\\\?\\", 4u) == 0) {
            memmove(canonical, canonical + 4u, strlen(canonical + 4u) + 1u);
        }
    }
#else
    char resolved[PATH_MAX];
    struct stat info;
    if (stat(path, &info) != 0) {
        module_path_error(loader, location, "cannot open module source", path);
        return NULL;
    }
    if (!S_ISREG(info.st_mode)) {
        module_error(loader, location, "module source must be a regular file");
        return NULL;
    }
    if (realpath(path, resolved) == NULL) {
        module_error(loader, location, "cannot canonicalize module source");
        return NULL;
    }
    canonical = module_copy(resolved, strlen(resolved));
#endif
    if (canonical == NULL) {
        module_error(loader, location, "out of memory canonicalizing module source");
    }
    return canonical;
}

static bool is_separator(char value) {
#ifdef _WIN32
    return value == '\\' || value == '/';
#else
    return value == '/';
#endif
}

#ifdef _WIN32
/* Compare using the filesystem's ordinal Unicode case rules, independent of
 * the C locale. Buffers cover the guarded native path limit and introduce no
 * allocation failures in graph identity/boundary checks. */
static bool windows_paths_match(const char *left, const char *right, bool prefix) {
    wchar_t native_left[32768];
    wchar_t native_right[32768];
    int i;
    int left_count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, left, -1,
                                         native_left, 32768);
    int right_count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, right, -1,
                                          native_right, 32768);
    if (left_count <= 1 || right_count <= 1) return false;
    /* Canonical display paths use '/', while filesystem candidates may use
     * '\\'. Compare the same native spelling without changing either stored
     * path or the path used for filesystem access. */
    for (i = 0; i < left_count; ++i)
        if (native_left[i] == L'/') native_left[i] = L'\\';
    for (i = 0; i < right_count; ++i)
        if (native_right[i] == L'/') native_right[i] = L'\\';
    if (!prefix) {
        return CompareStringOrdinal(native_left, left_count - 1, native_right,
                                     right_count - 1, TRUE) == CSTR_EQUAL;
    }
    if (right_count < left_count ||
        CompareStringOrdinal(native_left, left_count - 1, native_right,
                              left_count - 1, TRUE) != CSTR_EQUAL) return false;
    return native_left[left_count - 2] == L'\\' ||
           native_left[left_count - 2] == L'/' ||
           native_right[left_count - 1] == L'\\' ||
           native_right[left_count - 1] == L'/';
}
#endif

static bool same_path(const char *left, const char *right) {
#ifdef _WIN32
    return windows_paths_match(left, right, false);
#else
    return strcmp(left, right) == 0;
#endif
}

static bool inside_root(const char *root, const char *path) {
#ifdef _WIN32
    return windows_paths_match(root, path, true);
#else
    size_t length = strlen(root);
    if (strncmp(root, path, length) != 0) return false;
    /* A filesystem root already ends in a separator. */
    return length != 0u && (is_separator(root[length - 1u]) ||
                             is_separator(path[length]));
#endif
}

static bool valid_name(const char *path, size_t length) {
    size_t i;
    bool first = true;
    if (path == NULL || length == 0u) return false;
    for (i = 0u; i < length; ++i) {
        unsigned char c = (unsigned char)path[i];
        if (c == '/') {
            if (first) return false;
            first = true;
        } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   c == '_' || (!first && c >= '0' && c <= '9')) {
            first = false;
        } else {
            return false;
        }
    }
    return !first;
}

static char *read_module(ModuleLoader *loader, const char *path,
                          SourceLocation location) {
    FILE *file;
    long size;
    char *source;
    size_t count;
    bool regular;
#ifdef _WIN32
    wchar_t *wide = native_path(path);
    if (wide == NULL) {
        module_error(loader, location, "out of memory opening module source");
        return NULL;
    }
    file = _wfopen(wide, L"rb");
    free(wide);
    regular = true; /* checked through the native handle before opening */
#else
    struct stat info;
    file = fopen(path, "rb");
    regular = file != NULL && fstat(fileno(file), &info) == 0 && S_ISREG(info.st_mode);
#endif
    if (file == NULL) {
        module_path_error(loader, location, "cannot open module source", path);
        return NULL;
    }
    if (!regular || fseek(file, 0L, SEEK_END) != 0 || (size = ftell(file)) < 0L ||
        (unsigned long)size > MODULE_BYTE_LIMIT - loader->bytes ||
        fseek(file, 0L, SEEK_SET) != 0) {
        module_error(loader, location, "module source exceeds aggregate 64 MiB limit or is not a regular seekable file");
        (void)fclose(file);
        return NULL;
    }
    source = (char *)malloc((size_t)size + 1u);
    if (source == NULL) {
        module_error(loader, location, "out of memory reading module source");
        (void)fclose(file);
        return NULL;
    }
    count = fread(source, 1u, (size_t)size, file);
    regular = ferror(file) == 0;
    if (fclose(file) != 0) regular = false;
    if (!regular || count != (size_t)size) {
        module_error(loader, location, "failed to read module source");
        free(source);
        return NULL;
    }
    if (memchr(source, '\0', count) != NULL) {
        module_error(loader, location, "raw NUL byte in module source");
        free(source);
        return NULL;
    }
    source[count] = '\0';
    loader->bytes += count;
    return source;
}

/* Retain the parser's filename before freeing its program shell. If the parser
 * returns NULL, its OOM diagnostic instead borrows the already-retained path. */
static AstProgram *parse_module(ModuleLoader *loader, size_t index,
                                 const char *source) {
    AstProgram *program = parser_parse(loader->result->source_files[index], source,
                                       loader->diagnostics);
    if (program != NULL) {
        free(loader->result->source_files[index]);
        loader->result->source_files[index] = program->filename;
        program->filename = NULL;
    }
    loader->nodes[index].program = program;
    if (program == NULL || program->had_parse_error ||
        (loader->diagnostics != NULL && diagnostics_has_errors(loader->diagnostics))) {
        loader->result->had_parse_error = true;
    }
    return program;
}

static bool resolve_module(ModuleLoader *loader, size_t index, unsigned depth);

static bool resolve_import(ModuleLoader *loader, const AstStmt *statement,
                            unsigned depth) {
    size_t root_length = strlen(loader->root);
    size_t length = statement->as.summon.path_length;
    char *candidate;
    char *canonical;
    char *source;
    size_t i;
    AstProgram *program;
    SourceLocation location = statement->span.start;
    if (!valid_name(statement->as.summon.path, length)) {
        module_error(loader, location, "invalid module path: expected slash-separated identifier segments without a suffix");
        return false;
    }
    if (length > SIZE_MAX - root_length - 7u) {
        module_error(loader, location, "module path is too long");
        return false;
    }
    candidate = (char *)malloc(root_length + length + 7u);
    if (candidate == NULL) {
        module_error(loader, location, "out of memory resolving module path");
        return false;
    }
    memcpy(candidate, loader->root, root_length);
    candidate[root_length] = '/';
    memcpy(candidate + root_length + 1u, statement->as.summon.path, length);
    memcpy(candidate + root_length + length + 1u, ".agne", 6u);
#ifdef _WIN32
    for (i = 0u; candidate[i] != '\0'; ++i) {
        if (candidate[i] == '/') candidate[i] = '\\';
    }
#endif
    canonical = canonical_file(loader, candidate, location);
    free(candidate);
    if (canonical == NULL) return false;
    if (!inside_root(loader->root, canonical)) {
        module_error(loader, location, "module target escapes the canonical entry-file root");
        free(canonical);
        return false;
    }
    for (i = 0u; i < loader->result->source_file_count; ++i) {
        if (same_path(loader->result->source_files[i], canonical)) {
            free(canonical);
            if (loader->nodes[i].state == 1u) {
                module_path_error(loader, location,
                    "module import cycle: target is already on the visiting stack",
                    loader->result->source_files[i]);
                return false;
            }
            return true;
        }
    }
    if (loader->result->source_file_count >= MODULE_LIMIT || depth >= MODULE_DEPTH_LIMIT) {
        module_error(loader, location, depth >= MODULE_DEPTH_LIMIT
            ? "module import depth limit exceeded (64)" : "module graph limit exceeded (256)");
        free(canonical);
        return false;
    }
    i = loader->result->source_file_count++;
    loader->result->source_files[i] = canonical;
    source = read_module(loader, canonical, location);
    if (source == NULL) return false;
#ifdef _WIN32
    /* The file was opened and validated with the native canonical spelling.
     * Normalize only after filesystem access, for diagnostics and source
     * locations retained by the parsed module. */
    normalize_display_path(canonical);
#endif
    program = parse_module(loader, i, source);
    free(source);
    if (program == NULL || loader->result->had_parse_error) return false;
    return resolve_module(loader, i, depth + 1u);
}

static bool resolve_module(ModuleLoader *loader, size_t index, unsigned depth) {
    AstProgram *program = loader->nodes[index].program;
    size_t i;
    loader->nodes[index].state = 1u;
    for (i = 0u; i < program->item_count; ++i) {
        AstTopLevel *item = &program->items[i];
        if (item->kind == AST_TOP_EXPRESSION && item->as.statement != NULL &&
            item->as.statement->kind == AST_STMT_SUMMON &&
            !resolve_import(loader, item->as.statement, depth)) return false;
    }
    for (i = 0u; i < program->item_count; ++i) {
        AstTopLevel *item = &program->items[i];
        if (item->kind == AST_TOP_EXPRESSION && item->as.statement != NULL &&
            item->as.statement->kind == AST_STMT_SUMMON) continue;
        if (!ast_program_add_item(loader->result, *item)) {
            module_error(loader, item->span.start, "out of memory linking module items");
            return false;
        }
        /* Ownership moved. NULL is accepted by AST destruction for both arms. */
        item->as.statement = NULL;
    }
    loader->nodes[index].state = 2u;
    return true;
}

AstProgram *aphelion_module_parse(const char *filename, const char *source,
                                 Diagnostics *diagnostics) {
    ModuleLoader loader;
    AstProgram *result = parser_parse(filename, source, diagnostics);
    AstProgram *root_program;
    SourceLocation location;
    char *canonical;
    size_t i, root_length;
    bool has_import = false;
    if (result == NULL || result->had_parse_error) return result;
    for (i = 0u; i < result->item_count; ++i) {
        if (result->items[i].kind == AST_TOP_EXPRESSION &&
            result->items[i].as.statement->kind == AST_STMT_SUMMON) has_import = true;
    }
    if (!has_import) return result;
    memset(&loader, 0, sizeof(loader));
    loader.result = result;
    loader.diagnostics = diagnostics;
    location.filename = result->filename;
    location.offset = 0u;
    location.line = location.column = 1u;
    loader.bytes = source != NULL ? strlen(source) : 0u;
    if (loader.bytes > MODULE_BYTE_LIMIT) {
        module_error(&loader, location, "module sources exceed aggregate 64 MiB limit");
        return result;
    }
    canonical = canonical_file(&loader, filename != NULL ? filename : "", location);
    if (canonical == NULL) return result;
    result->source_files = (char **)calloc(MODULE_LIMIT, sizeof(*result->source_files));
    if (result->source_files == NULL) {
        free(canonical);
        module_error(&loader, location, "out of memory creating module graph");
        return result;
    }
    result->source_file_capacity = MODULE_LIMIT;
    result->source_files[result->source_file_count++] = canonical;
    root_length = strlen(canonical);
    while (root_length != 0u && !is_separator(canonical[root_length - 1u])) --root_length;
    /* Preserve the separator for / and Windows drive roots. */
    if (root_length > 1u && canonical[root_length - 2u] != ':') --root_length;
    loader.root = module_copy(canonical, root_length);
    if (loader.root == NULL) {
        module_error(&loader, location, "out of memory creating module root");
        return result;
    }
#ifdef _WIN32
    normalize_display_path(result->source_files[0]);
#endif
    /* Parse the entry under its retained canonical identity. The first parse
     * makes no-import/in-memory clients independent of filesystem setup. */
    root_program = parse_module(&loader, 0u, source);
    if (root_program != NULL && !result->had_parse_error) {
        for (i = 0u; i < result->item_count; ++i) {
            if (result->items[i].kind == AST_TOP_FUNCTION)
                ast_function_free(result->items[i].as.function);
            else ast_stmt_free(result->items[i].as.statement);
        }
        result->item_count = 0u;
        (void)resolve_module(&loader, 0u, 1u);
    }
    for (i = 0u; i < result->source_file_count; ++i) {
        ast_program_free(loader.nodes[i].program);
    }
    free(loader.root);
    return result;
}
