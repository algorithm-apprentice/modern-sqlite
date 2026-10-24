#ifndef MODERN_SQLITE_BENCHMARKS_NATIVE_PARSER_ADAPTER_H_
#define MODERN_SQLITE_BENCHMARKS_NATIVE_PARSER_ADAPTER_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ModernSqliteNativeParser ModernSqliteNativeParser;

typedef struct {
  int code;
  int errors;
  int has_vdbe;
  size_t consumed;
} ModernSqliteNativeParseResult;

typedef struct {
  uint64_t malloc_count_current;
  uint64_t malloc_count_highwater;
  uint64_t memory_bytes_current;
  uint64_t memory_bytes_highwater;
  uint64_t lookaside_used;
  uint64_t lookaside_highwater;
  uint64_t lookaside_hits;
} ModernSqliteNativeParserStatus;

int ModernSqliteNativeParserCreate(ModernSqliteNativeParser** output);
int ModernSqliteNativeParserDestroy(ModernSqliteNativeParser* parser);
/* The borrowed input has an accessible NUL terminator at input[length]. */
int ModernSqliteNativeParserRun(ModernSqliteNativeParser* parser, const char* input, size_t length,
                                ModernSqliteNativeParseResult* output);
int ModernSqliteNativeParserReadStatus(ModernSqliteNativeParser* parser, int reset,
                                       ModernSqliteNativeParserStatus* output);
int ModernSqliteNativeParserFailAfter(int allocations);
const char* ModernSqliteNativeParserSourceId(void);

#ifdef __cplusplus
}
#endif

#endif
