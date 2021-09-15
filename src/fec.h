#pragma once

#include "memory.h"
#include "urlopen.h"
#include "writer.h"

typedef ssize_t (*GetLine)(STRING *line, void *file);
typedef int (*PutLine)(char *line, void *file);

struct fec_context
{
  // A way to pull line info
  GetLine getLine;
  void *file;

  WRITE_CONTEXT *writeContext;

  char *version; // default null
  int versionLength;
  int useAscii28;
  int summary; // default false
  char *f99Text;

  // Supporting line information
  PERSISTENT_MEMORY_CONTEXT *persistentMemory;
  int currentLineHasAscii28;

  // Parse cache
  char *formType;
  int numFields;
  char *headers; // pointer to static CSV header row info
  char *types;   // dynamically allocated string where each char indicates types
};
typedef struct fec_context FEC_CONTEXT;

FEC_CONTEXT *newFecContext(PERSISTENT_MEMORY_CONTEXT *persistentMemory, GetLine getLine, void *file, char *filingId, char *outputDirectory);

void freeFecContext(FEC_CONTEXT *context);

int parseFec(FEC_CONTEXT *ctx);