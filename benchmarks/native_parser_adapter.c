#define SQLITE_API static
#define SQLITE_EXTERN static
static int modern_syntax_only;
#include "native_parser_adapter.h"

#include "native_parser_sqlite.c"

struct ModernSqliteNativeParser {
  sqlite3* database;
};

#if MODERN_SQLITE_READ_PERFORMANCE_DIAGNOSTICS
static sqlite3_mem_methods modern_parser_allocator;
static int modern_parser_allocator_configured;
static int modern_parser_fail_after = -1;

static int modernParserShouldFail(void) {
  if (modern_parser_fail_after < 0) return 0;
  if (modern_parser_fail_after == 0) return 1;
  --modern_parser_fail_after;
  return 0;
}

static void* modernParserMalloc(int size) {
  return modernParserShouldFail() ? 0 : modern_parser_allocator.xMalloc(size);
}

static void* modernParserRealloc(void* allocation, int size) {
  return modernParserShouldFail() ? 0 : modern_parser_allocator.xRealloc(allocation, size);
}

static int modernParserConfigureAllocator(void) {
  sqlite3_mem_methods methods;
  int code;
  if (modern_parser_allocator_configured) return SQLITE_OK;
  code = sqlite3_config(SQLITE_CONFIG_GETMALLOC, &modern_parser_allocator);
  if (code != SQLITE_OK) return code;
  methods = modern_parser_allocator;
  methods.xMalloc = modernParserMalloc;
  methods.xRealloc = modernParserRealloc;
  code = sqlite3_config(SQLITE_CONFIG_MALLOC, &methods);
  if (code == SQLITE_OK) modern_parser_allocator_configured = 1;
  return code;
}
#endif

int ModernSqliteNativeParserCreate(ModernSqliteNativeParser** output) {
  ModernSqliteNativeParser* parser;
  sqlite3* database = 0;
  int code;
  if (output == 0) return SQLITE_MISUSE;
  *output = 0;
#if MODERN_SQLITE_READ_PERFORMANCE_DIAGNOSTICS
  code = modernParserConfigureAllocator();
  if (code != SQLITE_OK) return code;
#endif
  code = sqlite3_open_v2(":memory:", &database,
                         SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, 0);
  if (code != SQLITE_OK) {
    if (database != 0) sqlite3_close(database);
    return code;
  }
  parser = sqlite3_malloc64(sizeof(*parser));
  if (parser == 0) {
    sqlite3_close(database);
    return SQLITE_NOMEM;
  }
  parser->database = database;
  *output = parser;
  return SQLITE_OK;
}

int ModernSqliteNativeParserDestroy(ModernSqliteNativeParser* parser) {
  int code;
  if (parser == 0) return SQLITE_OK;
  code = sqlite3_close(parser->database);
  if (code == SQLITE_OK) sqlite3_free(parser);
  return code;
}

int ModernSqliteNativeParserRun(ModernSqliteNativeParser* parser, const char* input, size_t length,
                                ModernSqliteNativeParseResult* output) {
  Parse parse = {0};
  const char* first = input;
  int token = TK_SPACE;
  int code;
  int disabled;
  int tables;
  int indexes;
  if (parser == 0 || input == 0 || output == 0 || length > INT_MAX || input[length] != 0) {
    return SQLITE_MISUSE;
  }
  while (*first != 0) {
    int bytes = sqlite3GetToken((const unsigned char*)first, &token);
    if (token != TK_SPACE && token != TK_SEMI && token != TK_COMMENT) break;
    first += bytes;
  }
  if (token != TK_SELECT) return SQLITE_MISUSE;
  disabled = parser->database->lookaside.bDisable;
  tables = parser->database->aDb[0].pSchema->tblHash.count;
  indexes = parser->database->aDb[0].pSchema->idxHash.count;
  sqlite3ParseObjectInit(&parse, parser->database);
  modern_syntax_only = 1;
  output->errors = sqlite3RunParser(&parse, input);
  modern_syntax_only = 0;
  output->code = parse.rc;
  output->has_vdbe = parse.pVdbe != 0;
  output->consumed = (size_t)(parse.zTail - input);
  if (parse.zErrMsg != 0) sqlite3DbFree(parser->database, parse.zErrMsg);
  if (parse.pVdbe != 0) sqlite3VdbeDelete(parse.pVdbe);
  sqlite3ParseObjectReset(&parse);
  output->code = sqlite3ApiExit(parser->database, output->code);
  code = output->has_vdbe || parser->database->lookaside.bDisable != disabled ||
                 parser->database->aDb[0].pSchema->tblHash.count != tables ||
                 parser->database->aDb[0].pSchema->idxHash.count != indexes ||
                 sqlite3_next_stmt(parser->database, 0) != 0
             ? SQLITE_INTERNAL
             : SQLITE_OK;
  return code;
}

int ModernSqliteNativeParserReadStatus(ModernSqliteNativeParser* parser, int reset,
                                       ModernSqliteNativeParserStatus* output) {
  sqlite3_int64 current;
  sqlite3_int64 highwater;
  int used;
  int peak;
  int code;
  if (parser == 0 || output == 0) return SQLITE_MISUSE;
  code = sqlite3_status64(SQLITE_STATUS_MALLOC_COUNT, &current, &highwater, reset);
  if (code != SQLITE_OK) return code;
  output->malloc_count_current = (uint64_t)current;
  output->malloc_count_highwater = (uint64_t)highwater;
  code = sqlite3_status64(SQLITE_STATUS_MEMORY_USED, &current, &highwater, reset);
  if (code != SQLITE_OK) return code;
  output->memory_bytes_current = (uint64_t)current;
  output->memory_bytes_highwater = (uint64_t)highwater;
  code = sqlite3_db_status(parser->database, SQLITE_DBSTATUS_LOOKASIDE_USED, &used, &peak, reset);
  if (code != SQLITE_OK) return code;
  output->lookaside_used = (uint64_t)used;
  output->lookaside_highwater = (uint64_t)peak;
  code = sqlite3_db_status(parser->database, SQLITE_DBSTATUS_LOOKASIDE_HIT, &used, &peak, reset);
  if (code != SQLITE_OK) return code;
  output->lookaside_hits = (uint64_t)peak;
  return SQLITE_OK;
}

int ModernSqliteNativeParserFailAfter(int allocations) {
#if MODERN_SQLITE_READ_PERFORMANCE_DIAGNOSTICS
  int code = modernParserConfigureAllocator();
  if (code != SQLITE_OK) return code;
  modern_parser_fail_after = allocations;
  return SQLITE_OK;
#else
  (void)allocations;
  return SQLITE_MISUSE;
#endif
}

const char* ModernSqliteNativeParserSourceId(void) { return sqlite3_sourceid(); }
