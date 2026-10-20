#include "aphelion/ast.h"
#include "aphelion/bytecode.h"
#include "aphelion/common.h"
#include "aphelion/compiler.h"
#include "aphelion/file.h"
#include "aphelion/lexer.h"
#include "aphelion/module.h"
#include "aphelion/parser.h"
#include "aphelion/semantic.h"
#include "aphelion/token.h"
#include "aphelion/version.h"
#include "aphelion/vm.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#include <shellapi.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

#define SOURCE_FILE_LIMIT (64u * 1024u * 1024u)
#define REPL_LINE_LIMIT (64u * 1024u)

static void print_usage(FILE *out) {
    (void)fputs(
        "Usage:\n"
        "  aphelion run file.agne\n"
        "  aphelion check file.agne\n"
        "  aphelion compile file.agne\n"
        "  aphelion tokens file.agne\n"
        "  aphelion ast file.agne\n"
        "  aphelion disasm file.apb\n"
        "  aphelion repl\n"
        "  aphelion --version\n"
        "  aphelion --help\n\n"
        "Source commands require the .agne extension.  compile writes the\n"
        "validated bytecode beside the source as .apb.\n",
        out);
}

static void print_common_diagnostics(const Diagnostics *diagnostics) {
    size_t i;
    if (diagnostics == NULL) return;
    if (diagnostics->allocation_failed)
        (void)fputs("<source>:1:1: out of memory recording diagnostics\n", stderr);
    for (i = 0u; i < diagnostics_count(diagnostics); ++i) {
        const Diagnostic *diagnostic = diagnostics_at(diagnostics, i);
        if (diagnostic == NULL) continue;
        (void)fprintf(stderr, "%s:%zu:%zu: %s\n",
                      diagnostic->location.filename != NULL
                        ? diagnostic->location.filename : "<source>",
                      diagnostic->location.line == 0u ? 1u : diagnostic->location.line,
                      diagnostic->location.column == 0u ? 1u : diagnostic->location.column,
                      diagnostic->message != NULL ? diagnostic->message : "<diagnostic>");
    }
}

static bool read_source_file(const char *path, char **source, size_t *length) {
    FILE *file;
    long size;
    char *buffer;
    size_t read_count;
    int close_result;
    int read_error;
    if (source == NULL || length == NULL || path == NULL) return false;
    *source = NULL;
    *length = 0u;
    if (!aphelion_file_is_regular(path)) {
        (void)fprintf(stderr, "%s:1:1: source must be an accessible regular file\n", path);
        return false;
    }
    file = aphelion_file_open(path, "rb");
    if (file == NULL) {
        (void)fprintf(stderr, "%s:1:1: cannot open source: %s\n", path,
                      strerror(errno));
        return false;
    }
    if (fseek(file, 0L, SEEK_END) != 0 || (size = ftell(file)) < 0L ||
        (unsigned long)size > SOURCE_FILE_LIMIT || fseek(file, 0L, SEEK_SET) != 0) {
        (void)fprintf(stderr, "%s:1:1: source is too large or not seekable\n", path);
        (void)fclose(file);
        return false;
    }
    buffer = (char *)malloc((size_t)size + 1u);
    if (buffer == NULL) {
        (void)fprintf(stderr, "%s:1:1: out of memory reading source\n", path);
        (void)fclose(file);
        return false;
    }
    read_count = fread(buffer, 1u, (size_t)size, file);
    read_error = ferror(file);
    close_result = fclose(file);
    buffer[read_count] = '\0';
    if (read_count != (size_t)size || read_error != 0 || close_result != 0) {
        (void)fprintf(stderr, "%s:1:1: failed to read source\n", path);
        free(buffer);
        return false;
    }
    if (memchr(buffer, '\0', read_count) != NULL) {
        size_t i, line = 1u, column = 1u;
        for (i = 0u; i < read_count && buffer[i] != '\0'; ++i) {
            if (buffer[i] == '\n') { ++line; column = 1u; } else { ++column; }
        }
        (void)fprintf(stderr, "%s:%zu:%zu: raw NUL byte in source\n", path, line, column);
        free(buffer);
        return false;
    }
    *source = buffer;
    *length = read_count;
    return true;
}

static bool has_extension(const char *path, const char *extension) {
    size_t path_length;
    size_t extension_length;
    if (path == NULL || extension == NULL) return false;
    path_length = strlen(path);
    extension_length = strlen(extension);
    return path_length >= extension_length &&
           strcmp(path + path_length - extension_length, extension) == 0;
}

static char *bytecode_path_for(const char *source_path) {
    size_t length;
    char *path;
    if (!has_extension(source_path, ".agne")) return NULL;
    length = strlen(source_path);
    path = (char *)malloc(length + 1u);
    if (path == NULL) return NULL;
    memcpy(path, source_path, length + 1u);
    memcpy(path + length - 5u, ".apb", 5u);
    return path;
}

static bool parse_and_check(const char *path, const char *source,
                            AstProgram **program_out) {
    Diagnostics diagnostics;
    AstProgram *program;
    bool ok;
    diagnostics_init(&diagnostics);
    program = aphelion_module_parse(path, source, &diagnostics);
    ok = program != NULL && !program->had_parse_error &&
         !diagnostics_has_errors(&diagnostics);
    if (ok) {
        ok = semantic_check(program, &diagnostics);
    }
    if (!ok || diagnostics_has_errors(&diagnostics)) {
        if (diagnostics_count(&diagnostics) == 0u && !diagnostics.allocation_failed) {
            (void)fprintf(stderr, "%s:1:1: front end failed without a diagnostic\n", path);
        } else {
            print_common_diagnostics(&diagnostics);
        }
        if (program != NULL) ast_program_free(program);
        diagnostics_free(&diagnostics);
        if (program_out != NULL) *program_out = NULL;
        return false;
    }
    diagnostics_free(&diagnostics);
    if (program_out != NULL) *program_out = program;
    else ast_program_free(program);
    return true;
}

static bool compile_source(const char *path, const char *source,
                           AphelionBytecode *bytecode) {
    AstProgram *program = NULL;
    AphelionDiagnostics diagnostics;
    bool ok;
    aphelion_diagnostics_init(&diagnostics);
    if (!parse_and_check(path, source, &program)) {
        aphelion_diagnostics_destroy(&diagnostics);
        return false;
    }
    ok = aphelion_compile_program(program, NULL, bytecode, &diagnostics);
    if (!ok || aphelion_diagnostics_has_errors(&diagnostics)) {
        aphelion_diagnostics_print(&diagnostics, stderr);
        ok = false;
    }
    aphelion_diagnostics_destroy(&diagnostics);
    ast_program_free(program);
    return ok;
}

static void print_token_lexeme(const Token *token) {
    size_t i;
    if (token == NULL || token->lexeme == NULL) return;
    for (i = 0u; i < token->lexeme_length; ++i) {
        unsigned char value = (unsigned char)token->lexeme[i];
        if (value == '\n') (void)fputs("\\n", stdout);
        else if (value == '\r') (void)fputs("\\r", stdout);
        else if (value == '\t') (void)fputs("\\t", stdout);
        else if (value < 32u || value == 127u) (void)fprintf(stdout, "\\x%02X", value);
        else (void)fputc((int)value, stdout);
    }
}

static int command_tokens(const char *path, const char *source) {
    Lexer lexer;
    Token token;
    Diagnostics diagnostics;
    bool had_errors;
    diagnostics_init(&diagnostics);
    lexer_init(&lexer, path, source);
    for (;;) {
        token = lexer_next(&lexer);
        (void)fprintf(stdout, "%s %zu:%zu `", token_kind_name(token.kind),
                      token.location.line == 0u ? 1u : token.location.line,
                      token.location.column == 0u ? 1u : token.location.column);
        print_token_lexeme(&token);
        (void)fprintf(stdout, "`\n");
        if (token.kind == TOKEN_EOF) break;
    }
    had_errors = lexer_has_errors(&lexer) != 0;
    diagnostics = *lexer_diagnostics(&lexer);
    if (had_errors) print_common_diagnostics(&diagnostics);
    /* lexer_diagnostics storage is owned by lexer; do not free the copy. */
    lexer_destroy(&lexer);
    return had_errors ? 1 : 0;
}

static void print_indent(size_t depth) {
    while (depth-- > 0u) (void)fputs("  ", stdout);
}

static const char *stmt_kind_name(AstStmtKind kind) {
    switch (kind) {
    case AST_STMT_VAR_DECL: return "declaration";
    case AST_STMT_EXPRESSION: return "expression";
    case AST_STMT_BLOCK: return "block";
    case AST_STMT_YIELD: return "yield";
    case AST_STMT_WHEN: return "when";
    case AST_STMT_CYCLE: return "cycle";
    case AST_STMT_ESCAPE: return "escape";
    case AST_STMT_SKIP: return "skip";
    case AST_STMT_TRAVERSE: return "traverse";
    case AST_STMT_SUMMON: return "summon";
    default: return "statement";
    }
}

static const char *expr_kind_name(AstExprKind kind) {
    switch (kind) {
    case AST_EXPR_LITERAL: return "literal";
    case AST_EXPR_NAME: return "name";
    case AST_EXPR_UNARY: return "unary";
    case AST_EXPR_BINARY: return "binary";
    case AST_EXPR_CALL: return "call";
    case AST_EXPR_ASSIGN: return "assign";
    case AST_EXPR_GROUP: return "group";
    case AST_EXPR_LIST: return "list";
    case AST_EXPR_INDEX: return "index";
    default: return "expression";
    }
}

static void print_expr(const AstExpr *expression, size_t depth);

static void print_block(const AstBlock *block, size_t depth) {
    size_t i;
    if (block == NULL) {
        print_indent(depth);
        (void)fputs("<missing block>\n", stdout);
        return;
    }
    print_indent(depth);
    (void)fprintf(stdout, "block (%zu statement%s)\n", block->statement_count,
                  block->statement_count == 1u ? "" : "s");
    for (i = 0u; i < block->statement_count; ++i) {
        const AstStmt *statement = block->statements[i];
        print_indent(depth + 1u);
        (void)fprintf(stdout, "%s\n", stmt_kind_name(statement->kind));
        switch (statement->kind) {
        case AST_STMT_VAR_DECL:
            print_indent(depth + 2u);
            (void)fprintf(stdout, "%s %s\n",
                          statement->as.var_decl.storage == AST_STORAGE_FORGE ? "forge" :
                          statement->as.var_decl.storage == AST_STORAGE_SHIFT ? "shift" : "anchor",
                          statement->as.var_decl.name != NULL ? statement->as.var_decl.name : "<unnamed>");
            print_expr(statement->as.var_decl.initializer, depth + 2u);
            break;
        case AST_STMT_EXPRESSION:
            print_expr(statement->as.expression.expression, depth + 2u);
            break;
        case AST_STMT_BLOCK:
            print_block(statement->as.block.block, depth + 2u);
            break;
        case AST_STMT_YIELD:
            print_expr(statement->as.yield.expression, depth + 2u);
            break;
        case AST_STMT_WHEN:
            print_indent(depth + 2u); (void)fputs("condition\n", stdout);
            print_expr(statement->as.when_stmt.condition, depth + 3u);
            print_indent(depth + 2u); (void)fputs("then\n", stdout);
            print_block(statement->as.when_stmt.then_branch, depth + 3u);
            if (statement->as.when_stmt.else_branch != NULL) {
                print_indent(depth + 2u); (void)fputs("otherwise\n", stdout);
                print_block(statement->as.when_stmt.else_branch, depth + 3u);
            }
            break;
        case AST_STMT_CYCLE:
            print_indent(depth + 2u); (void)fputs("condition\n", stdout);
            print_expr(statement->as.cycle.condition, depth + 3u);
            print_block(statement->as.cycle.body, depth + 2u);
            break;
        case AST_STMT_TRAVERSE: {
            const AstStmt *binding = statement->as.traverse.binding;
            print_indent(depth + 2u);
            (void)fprintf(stdout, "binding forge %s\n",
                          binding != NULL && binding->as.var_decl.name != NULL
                              ? binding->as.var_decl.name : "<missing>");
            print_indent(depth + 2u); (void)fputs("iterable\n", stdout);
            print_expr(statement->as.traverse.iterable, depth + 3u);
            print_block(statement->as.traverse.body, depth + 2u);
            break;
        }
        case AST_STMT_ESCAPE:
        case AST_STMT_SKIP:
            break;
        case AST_STMT_SUMMON:
            print_indent(depth + 2u);
            (void)fprintf(stdout, "%s\n", statement->as.summon.path);
            break;
        }
    }
}

static void print_expr(const AstExpr *expression, size_t depth) {
    size_t i;
    if (expression == NULL) {
        print_indent(depth); (void)fputs("<missing expression>\n", stdout); return;
    }
    print_indent(depth);
    (void)fprintf(stdout, "%s", expr_kind_name(expression->kind));
    if (expression->kind == AST_EXPR_NAME) {
        (void)fprintf(stdout, " %s", expression->as.name.name != NULL
                                     ? expression->as.name.name : "<unnamed>");
    } else if (expression->kind == AST_EXPR_LITERAL) {
        AphelionValue runtime_value;
        (void)fputc(' ', stdout);
        if (expression->as.literal.kind == AST_LITERAL_TEXT) {
            for (i = 0u; i < expression->as.literal.text_length; ++i) {
                unsigned char byte = (unsigned char)expression->as.literal.text[i];
                if (byte == '\0') (void)fputs("\\0", stdout);
                else (void)fputc(byte, stdout);
            }
        } else {
            switch (expression->as.literal.kind) {
            case AST_LITERAL_WHOLE: runtime_value = aphelion_value_whole(expression->as.literal.whole); break;
            case AST_LITERAL_DECIMAL: runtime_value = aphelion_value_decimal(expression->as.literal.decimal); break;
            case AST_LITERAL_SIGNAL: runtime_value = aphelion_value_signal(expression->as.literal.signal); break;
            case AST_LITERAL_GLYPH: runtime_value = aphelion_value_glyph(expression->as.literal.glyph); break;
            default: runtime_value = aphelion_value_hollow(); break;
            }
            (void)aphelion_value_write(stdout, runtime_value);
            aphelion_value_destroy(&runtime_value);
        }
    } else if (expression->kind == AST_EXPR_LIST) {
        (void)fprintf(stdout, " (%zu elements)", expression->as.list.element_count);
    }
    (void)fputc('\n', stdout);
    switch (expression->kind) {
    case AST_EXPR_LIST:
        for (i = 0u; i < expression->as.list.element_count; ++i)
            print_expr(expression->as.list.elements[i], depth + 1u);
        break;
    case AST_EXPR_INDEX:
        print_indent(depth + 1u); (void)fputs("target\n", stdout);
        print_expr(expression->as.index.target, depth + 2u);
        print_indent(depth + 1u); (void)fputs("offset\n", stdout);
        print_expr(expression->as.index.index, depth + 2u);
        break;
    case AST_EXPR_UNARY: print_expr(expression->as.unary.operand, depth + 1u); break;
    case AST_EXPR_BINARY:
        print_expr(expression->as.binary.left, depth + 1u);
        print_expr(expression->as.binary.right, depth + 1u);
        break;
    case AST_EXPR_CALL:
        print_expr(expression->as.call.callee, depth + 1u);
        for (i = 0u; i < expression->as.call.argument_count; ++i)
            print_expr(expression->as.call.arguments[i], depth + 1u);
        break;
    case AST_EXPR_ASSIGN:
        print_expr(expression->as.assignment.target, depth + 1u);
        print_expr(expression->as.assignment.value, depth + 1u);
        break;
    case AST_EXPR_GROUP: print_expr(expression->as.group.inner, depth + 1u); break;
    case AST_EXPR_LITERAL:
    case AST_EXPR_NAME:
        break;
    }
}

static int command_ast(const char *path, const char *source) {
    Diagnostics diagnostics;
    AstProgram *program;
    size_t i;
    diagnostics_init(&diagnostics);
    program = aphelion_module_parse(path, source, &diagnostics);
    if (program == NULL || program->had_parse_error || diagnostics_has_errors(&diagnostics)) {
        print_common_diagnostics(&diagnostics);
        if (program != NULL) ast_program_free(program);
        diagnostics_free(&diagnostics);
        return 1;
    }
    (void)fprintf(stdout, "program %s (%zu item%s)\n", path, program->item_count,
                  program->item_count == 1u ? "" : "s");
    for (i = 0u; i < program->item_count; ++i) {
        const AstTopLevel *item = &program->items[i];
        if (item->kind == AST_TOP_FUNCTION) {
            size_t p;
            print_indent(1u);
            (void)fprintf(stdout, "orbit %s (", item->as.function->name);
            for (p = 0u; p < item->as.function->parameter_count; ++p) {
                if (p != 0u) (void)fputs(", ", stdout);
                (void)fprintf(stdout, "%s: %s", item->as.function->parameters[p].name,
                              type_kind_name(item->as.function->parameters[p].type.kind));
            }
            (void)fprintf(stdout, ") -> %s\n",
                          type_kind_name(item->as.function->return_type.kind));
            print_block(item->as.function->body, 2u);
        } else {
            AstStmt *statement = item->as.statement;
            AstBlock fake_block;
            print_indent(1u);
            (void)fprintf(stdout, "%s\n", item->kind == AST_TOP_DECLARATION ? "declaration" : "expression");
            memset(&fake_block, 0, sizeof(fake_block));
            fake_block.statements = &statement;
            fake_block.statement_count = 1u;
            print_block(&fake_block, 2u);
        }
    }
    ast_program_free(program);
    diagnostics_free(&diagnostics);
    return 0;
}

static int command_source(const char *command, const char *path) {
    char *source = NULL;
    size_t source_length = 0u;
    AphelionBytecode bytecode;
    int result = 1;
    if (!has_extension(path, ".agne")) {
        (void)fprintf(stderr, "%s:1:1: source commands require a .agne file\n", path);
        return 1;
    }
    if (!read_source_file(path, &source, &source_length)) return 1;
    (void)source_length;
    if (strcmp(command, "tokens") == 0) {
        result = command_tokens(path, source);
    } else if (strcmp(command, "ast") == 0) {
        result = command_ast(path, source);
    } else {
        aphelion_bytecode_init(&bytecode);
        if (compile_source(path, source, &bytecode)) {
            if (strcmp(command, "check") == 0) {
                (void)fprintf(stdout, "%s: valid\n", path);
                result = 0;
            } else if (strcmp(command, "compile") == 0) {
                char *output_path = bytecode_path_for(path);
                char error[256];
                if (output_path == NULL || !aphelion_bytecode_write_file(&bytecode, output_path,
                                                                         error, sizeof(error))) {
                    (void)fprintf(stderr, "%s:1:1: %s\n", path,
                                  output_path == NULL ? "out of memory creating .apb path" : error);
                    result = 1;
                } else {
                    (void)fprintf(stdout, "%s\n", output_path);
                    result = 0;
                }
                free(output_path);
            } else if (strcmp(command, "run") == 0) {
                AphelionVM vm;
                const AphelionRuntimeError *error;
                aphelion_vm_init(&vm);
                result = aphelion_vm_run(&vm, &bytecode) ? 0 : 1;
                error = aphelion_vm_error(&vm);
                if (result != 0 && error != NULL) {
                    (void)fprintf(stderr, "%s:%" PRIu32 ":%" PRIu32 ": runtime error: %s\n",
                                  error->file != NULL ? error->file : path,
                                  error->line == 0u ? 1u : error->line,
                                  error->column == 0u ? 1u : error->column,
                                  error->message[0] != '\0' ? error->message : "unknown error");
                }
                aphelion_vm_destroy(&vm);
            } else {
                (void)fprintf(stderr, "%s:1:1: unsupported source command '%s'\n", path, command);
            }
        }
        aphelion_bytecode_destroy(&bytecode);
    }
    free(source);
    return result;
}

static int command_disasm(const char *path) {
    AphelionBytecode bytecode;
    char error[256];
    aphelion_bytecode_init(&bytecode);
    if (!aphelion_bytecode_read_file(&bytecode, path, error, sizeof(error))) {
        (void)fprintf(stderr, "%s:1:1: %s\n", path, error);
        aphelion_bytecode_destroy(&bytecode);
        return 1;
    }
    if (!aphelion_bytecode_disassemble(&bytecode, stdout)) {
        (void)fprintf(stderr, "%s:1:1: disassembly output failed\n", path);
        aphelion_bytecode_destroy(&bytecode);
        return 1;
    }
    aphelion_bytecode_destroy(&bytecode);
    return 0;
}

/* Inspect tokens, rather than text, so strings/comments may contain summon.
 * Never use the file module loader for a REPL input. */
static bool repl_check_tokens(const char *source) {
    Lexer lexer;
    Token token;
    bool ok = true;
    lexer_init(&lexer, "<repl>", source);
    for (;;) {
        token = lexer_next(&lexer);
        if (token.kind == TOKEN_KW_SUMMON) {
            (void)fprintf(stderr, "<repl>:%zu:%zu: summon is file-only; use aphelion run file.agne\n",
                          token.location.line, token.location.column);
            ok = false;
            break;
        }
        if (token.kind == TOKEN_EOF) break;
    }
    if (lexer_has_errors(&lexer)) {
        print_common_diagnostics(lexer_diagnostics(&lexer));
        ok = false;
    }
    lexer_destroy(&lexer);
    return ok;
}

static bool repl_evaluate(const char *source) {
    Diagnostics diagnostics;
    AphelionDiagnostics compiler_diagnostics;
    AphelionCompilerOptions options;
    AphelionBytecode bytecode;
    AphelionVM vm;
    AstProgram *program;
    bool ok;
    const AphelionRuntimeError *error;
    if (!repl_check_tokens(source)) return false;
    diagnostics_init(&diagnostics);
    aphelion_diagnostics_init(&compiler_diagnostics);
    aphelion_bytecode_init(&bytecode);
    aphelion_vm_init(&vm);
    program = parser_parse("<repl>", source, &diagnostics);
    ok = program != NULL && !program->had_parse_error &&
         !diagnostics_has_errors(&diagnostics);
    if (ok) ok = semantic_check(program, &diagnostics);
    if (!ok || diagnostics_has_errors(&diagnostics)) {
        if (diagnostics_count(&diagnostics) == 0u && !diagnostics.allocation_failed) {
            (void)fputs("<repl>:1:1: front end failed without a diagnostic\n", stderr);
        } else {
            print_common_diagnostics(&diagnostics);
        }
        ok = false;
    } else {
        aphelion_compiler_options_default(&options);
        options.echo_expressions = true;
        ok = aphelion_compile_program(program, &options, &bytecode, &compiler_diagnostics);
        if (!ok || aphelion_diagnostics_has_errors(&compiler_diagnostics)) {
            aphelion_diagnostics_print(&compiler_diagnostics, stderr);
            ok = false;
        } else {
            /* receive() deliberately consumes the same stream as the loop. */
            aphelion_vm_set_input(&vm, stdin);
            ok = aphelion_vm_run(&vm, &bytecode);
            error = aphelion_vm_error(&vm);
            if (!ok && error != NULL) {
                (void)fprintf(stderr, "%s:%" PRIu32 ":%" PRIu32 ": runtime error: %s\n",
                              error->file != NULL ? error->file : "<repl>",
                              error->line == 0u ? 1u : error->line,
                              error->column == 0u ? 1u : error->column,
                              error->message[0] != '\0' ? error->message : "unknown error");
            }
        }
    }
    aphelion_vm_destroy(&vm);
    aphelion_bytecode_destroy(&bytecode);
    aphelion_diagnostics_destroy(&compiler_diagnostics);
    ast_program_free(program);
    diagnostics_free(&diagnostics);
    return ok;
}

typedef enum ReplInputResult {
    REPL_INPUT_END,
    REPL_INPUT_LINE,
    REPL_INPUT_REJECTED,
    REPL_INPUT_ERROR
} ReplInputResult;

static ReplInputResult repl_read_line(size_t line, char **source) {
    char *buffer;
    size_t length = 0u;
    size_t nul_column = 0u;
    bool overlong = false;
    int byte = fgetc(stdin);
    *source = NULL;
    if (byte == EOF) {
        if (!ferror(stdin)) return REPL_INPUT_END;
        (void)fprintf(stderr, "<repl>:%zu:1: input failed\n", line);
        return REPL_INPUT_ERROR;
    }
    buffer = (char *)malloc(REPL_LINE_LIMIT + 1u);
    if (buffer == NULL)
        (void)fprintf(stderr, "<repl>:%zu:1: out of memory reading input\n", line);
    /* Read exactly one physical line, including on rejection. Do not read
     * ahead: the following line might belong to receive(), not the REPL. */
    while (byte != '\n' && byte != EOF) {
        if (byte == 0 && nul_column == 0u) nul_column = length + 1u;
        if (length < REPL_LINE_LIMIT) {
            if (buffer != NULL) buffer[length] = (char)byte;
            ++length;
        } else {
            overlong = true;
        }
        byte = fgetc(stdin);
    }
    if (byte == EOF && ferror(stdin)) {
        (void)fprintf(stderr, "<repl>:%zu:1: input failed\n", line);
        free(buffer);
        return REPL_INPUT_ERROR;
    }
    if (nul_column != 0u)
        (void)fprintf(stderr, "<repl>:%zu:%zu: raw NUL byte in input\n", line, nul_column);
    if (overlong)
        (void)fprintf(stderr, "<repl>:%zu:1: input line exceeds 64 KiB source limit\n", line);
    if (buffer == NULL || nul_column != 0u || overlong) {
        free(buffer);
        return REPL_INPUT_REJECTED;
    }
    buffer[length] = '\0';
    *source = buffer;
    return REPL_INPUT_LINE;
}

static bool repl_flush_output(void) {
    if (fflush(stdout) == EOF || ferror(stdout)) {
        (void)fputs("aphelion: REPL output failed\n", stderr);
        return false;
    }
    return true;
}

static bool repl_command_is(const char *text, size_t length, const char *command) {
    return strlen(command) == length && memcmp(text, command, length) == 0;
}

static int command_repl(void) {
    bool interactive;
    bool failed = false;
    size_t line = 0u;
#ifdef _WIN32
    interactive = _isatty(_fileno(stdin)) != 0;
#else
    interactive = isatty(STDIN_FILENO) != 0;
    /* A closed output pipe is an I/O failure, not an asynchronous exit. */
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        (void)fputs("aphelion: cannot initialize REPL output handling\n", stderr);
        return 1;
    }
#endif
    if (interactive) {
        (void)fputs("Aphelion " APHELION_VERSION " REPL (:help for help, :quit to exit)\n", stdout);
        if (!repl_flush_output()) return 1;
    }
    for (;;) {
        char *source;
        const char *command;
        size_t length;
        ReplInputResult input;
        bool quit = false;
        if (interactive) {
            (void)fputs("aphelion> ", stdout);
            if (!repl_flush_output()) return 1;
        }
        input = repl_read_line(++line, &source);
        if (input == REPL_INPUT_END) break;
        if (input == REPL_INPUT_ERROR) return 1;
        if (input == REPL_INPUT_REJECTED) {
            failed = true;
            continue;
        }
        command = source + strspn(source, " \t\r\v\f");
        length = strlen(command);
        while (length != 0u && strchr(" \t\r\v\f", command[length - 1u]) != NULL)
            --length;
        if (repl_command_is(command, length, ":quit") ||
            repl_command_is(command, length, ":q")) {
            quit = true;
        } else if (repl_command_is(command, length, ":help")) {
            (void)fputs(
                "Each physical line is a fresh program/scope; declarations do not persist.\n"
                "Use semicolons/braces for related statements on one line.\n"
                "No multiline input; the source limit is 64 KiB per line.\n"
                "Expressions are echoed; silent results are not echoed.\n"
                "summon is file-only. receive() reads from the same stdin stream.\n"
                ":help shows this help; :quit or :q exits. EOF also exits.\n", stdout);
        } else if (length != 0u && command[0] == ':') {
            (void)fprintf(stderr, "<repl>:%zu:1: unknown REPL command; use :help\n", line);
            failed = true;
        } else if (length != 0u && !repl_evaluate(source)) {
            failed = true;
        }
        free(source);
        if (!repl_flush_output()) return 1;
        if (quit) break;
    }
    return failed ? 1 : 0;
}

static int cli_main(int argc, char **argv) {
    if (argc == 2 && (strcmp(argv[1], "--version") == 0 ||
                      strcmp(argv[1], "-V") == 0)) {
        (void)printf("aphelion %s\n", APHELION_VERSION);
        return 0;
    }
    if (argc == 2 && (strcmp(argv[1], "--help") == 0 ||
                      strcmp(argv[1], "-h") == 0)) {
        print_usage(stdout);
        return 0;
    }
    if (argc == 3 && (strcmp(argv[1], "run") == 0 ||
                      strcmp(argv[1], "check") == 0 ||
                      strcmp(argv[1], "compile") == 0 ||
                      strcmp(argv[1], "tokens") == 0 ||
                      strcmp(argv[1], "ast") == 0)) {
        return command_source(argv[1], argv[2]);
    }
    if (argc == 3 && strcmp(argv[1], "disasm") == 0) {
        if (!has_extension(argv[2], ".apb")) {
            (void)fprintf(stderr, "%s:1:1: disasm requires a .apb file\n", argv[2]);
            return 1;
        }
        return command_disasm(argv[2]);
    }
    if (argc == 2 && strcmp(argv[1], "repl") == 0) return command_repl();
    if (argc >= 2 && strcmp(argv[1], "receive") == 0) {
        (void)fprintf(stderr, "aphelion: %s is not implemented in this release\n", argv[1]);
        return 2;
    }
    print_usage(stderr);
    return 2;
}

#ifdef _WIN32
static void windows_free_arguments(int argc, char **argv) {
    int i;
    if (argv == NULL) return;
    for (i = 0; i < argc; ++i) free(argv[i]);
    free(argv);
}

/* Only the native entry point decodes the process command line. Internal CLI
 * callers already supply UTF-8, including test drivers with synthetic argv. */
static bool windows_utf8_arguments(int *argc_out, char ***argv_out) {
    int argc = 0;
    int i;
    wchar_t **wide_argv;
    char **argv = NULL;
    const char *error = "cannot decode native process arguments";
    *argc_out = 0;
    *argv_out = NULL;
    wide_argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (wide_argv == NULL) {
        DWORD code = GetLastError();
        if (code == ERROR_NOT_ENOUGH_MEMORY || code == ERROR_OUTOFMEMORY)
            error = "out of memory reading process arguments";
        goto failed;
    }
    if (argc <= 0) goto failed;
    if ((size_t)argc > SIZE_MAX / sizeof(*argv) - 1u) {
        error = "out of memory reading process arguments";
        goto failed;
    }
    /* A terminating NULL also makes cleanup safe after partial conversion. */
    argv = (char **)calloc((size_t)argc + 1u, sizeof(*argv));
    if (argv == NULL) {
        error = "out of memory reading process arguments";
        goto failed;
    }
    for (i = 0; i < argc; ++i) {
        int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                      wide_argv[i], -1, NULL, 0, NULL, NULL);
        if (size == 0) {
            if (GetLastError() == ERROR_NO_UNICODE_TRANSLATION)
                error = "process arguments contain invalid UTF-16";
            goto failed;
        }
        argv[i] = (char *)malloc((size_t)size);
        if (argv[i] == NULL) {
            error = "out of memory reading process arguments";
            goto failed;
        }
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_argv[i],
                                -1, argv[i], size, NULL, NULL) != size) {
            if (GetLastError() == ERROR_NO_UNICODE_TRANSLATION)
                error = "process arguments contain invalid UTF-16";
            goto failed;
        }
    }
    (void)LocalFree(wide_argv);
    *argc_out = argc;
    *argv_out = argv;
    return true;

failed:
    windows_free_arguments(argc, argv);
    if (wide_argv != NULL) (void)LocalFree(wide_argv);
    (void)fprintf(stderr, "aphelion: %s\n", error);
    return false;
}
#endif

int main(int argc, char **argv) {
    int result;
#ifdef _WIN32
    if (!windows_utf8_arguments(&argc, &argv)) return 1;
#endif
    result = cli_main(argc, argv);
#ifdef _WIN32
    windows_free_arguments(argc, argv);
#endif
    if (fflush(stdout) == EOF || ferror(stdout)) {
        (void)fputs("aphelion: output failed\n", stderr);
        return 1;
    }
    return result;
}
