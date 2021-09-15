#include "fec.h"
#include "encoding.h"
#include "writer.h"
#include "csv.h"
#include "mappings.h"
#include <string.h>

char *HEADER = "header";
char *SCHEDULE_COUNTS = "SCHEDULE_COUNTS_";
char *FEC_VERSION_NUMBER = "fec_ver_#";
char *FEC = "FEC";

char *COMMA_FEC_VERSIONS[] = {"1", "2", "3", "5"};
int NUM_COMMA_FEC_VERSIONS = sizeof(COMMA_FEC_VERSIONS) / sizeof(char *);

FEC_CONTEXT *newFecContext(PERSISTENT_MEMORY_CONTEXT *persistentMemory, GetLine getLine, void *file, char *filingId, char *outputDirectory)
{
  FEC_CONTEXT *ctx = (FEC_CONTEXT *)malloc(sizeof(FEC_CONTEXT));
  ctx->persistentMemory = persistentMemory;
  ctx->getLine = getLine;
  ctx->file = file;
  ctx->writeContext = newWriteContext(outputDirectory, filingId);
  ctx->version = 0;
  ctx->versionLength = 0;
  ctx->useAscii28 = 0; // default to using comma parsing unless a version is set
  ctx->summary = 0;
  ctx->f99Text = 0;
  ctx->currentLineHasAscii28 = 0;
  ctx->formType = NULL;
  ctx->numFields = 0;
  ctx->headers = NULL;
  ctx->types = NULL;
  return ctx;
}

void freeFecContext(FEC_CONTEXT *ctx)
{
  if (ctx->version)
  {
    free(ctx->version);
  }
  if (ctx->f99Text)
  {
    free(ctx->f99Text);
  }
  if (ctx->formType != NULL)
  {
    free(ctx->formType);
  }
  if (ctx->types != NULL)
  {
    free(ctx->types);
  }
  freeWriteContext(ctx->writeContext);
  free(ctx);
}

int isParseDone(PARSE_CONTEXT *parseContext)
{
  // The parse is done if a newline is encountered or EOF
  char c = parseContext->line->str[parseContext->position];
  return (c == 0) || (c == '\n');
}

void lookupMappings(FEC_CONTEXT *ctx, PARSE_CONTEXT *parseContext, int formStart, int formEnd)
{
  if ((ctx->formType != NULL) && (strncmp(ctx->formType, parseContext->line->str + formStart, formEnd - formStart) == 0))
  {
    // Type mappings are unchanged from before; can return early
    return;
  }

  // Clear last form type information if present
  if (ctx->formType != NULL)
  {
    free(ctx->formType);
  }
  // Set last form type to store it for later
  ctx->formType = malloc(formEnd - formStart + 1);
  strncpy(ctx->formType, parseContext->line->str + formStart, formEnd - formStart);
  ctx->formType[formEnd - formStart] = 0;

  // Grab the field mapping given the form version
  for (int i = 0; i < numHeaders; i++)
  {
    // Try to match the regex to version
    if (pcre_exec(ctx->persistentMemory->headerVersions[i], NULL, ctx->version, ctx->versionLength, 0, 0, NULL, 0) >= 0)
    {
      // Match! Test regex against form type
      if (pcre_exec(ctx->persistentMemory->headerFormTypes[i], NULL, parseContext->line->str + formStart, formEnd - formStart, 0, 0, NULL, 0) >= 0)
      {
        // Matched form type
        ctx->headers = (char *)(headers[i][2]);
        STRING *headersCsv = fromString(ctx->headers);
        if (ctx->types != NULL)
        {
          free(ctx->types);
        }
        ctx->numFields = 0;
        ctx->types = malloc(strlen(ctx->headers) + 1); // at least as big as it needs to be

        // Initialize a parse context for reading each header field
        PARSE_CONTEXT headerFields;
        headerFields.line = headersCsv;
        headerFields.fieldInfo = NULL;
        headerFields.position = 0;
        headerFields.start = 0;
        headerFields.end = 0;
        headerFields.columnIndex = 0;

        // Iterate each field and build up the type info
        while (!isParseDone(&headerFields))
        {
          readCsvField(&headerFields);

          // Match type info
          int matched = 0;
          for (int j = 0; j < numTypes; j++)
          {
            // Try to match the type regex to version
            if (pcre_exec(ctx->persistentMemory->typeVersions[j], NULL, ctx->version, ctx->versionLength, 0, 0, NULL, 0) >= 0)
            {
              // Try to match type regex to form type
              if (pcre_exec(ctx->persistentMemory->typeFormTypes[j], NULL, parseContext->line->str + formStart, formEnd - formStart, 0, 0, NULL, 0) >= 0)
              {
                // Try to match type regex to header
                if (pcre_exec(ctx->persistentMemory->typeHeaders[j], NULL, headerFields.line->str + headerFields.start, headerFields.end - headerFields.start, 0, 0, NULL, 0) >= 0)
                {
                  // Match! Print out type information
                  ctx->types[headerFields.columnIndex] = types[j][3][0];
                  matched = 1;
                  break;
                }
              }
            }
          }

          if (!matched)
          {
            // Unmatched type — default to 's' for string type
            ctx->types[headerFields.columnIndex] = 's';
          }

          if (isParseDone(&headerFields))
          {
            break;
          }
          advanceField(&headerFields);
        }

        // Add null terminator
        ctx->types[headerFields.columnIndex] = 0;
        ctx->numFields = headerFields.columnIndex;

        // Free up unnecessary line memory
        freeString(headersCsv);

        // Done; return
        return;
      }
    }
  }

  // Unmatched — error
  printf("Error: Unmatched for version %s and form type %s\n", ctx->version, ctx->formType);
  exit(1);
}

void writeSubstrToWriter(FEC_CONTEXT *ctx, WRITE_CONTEXT *writeContext, char *filename, int start, int end, FIELD_INFO *field)
{
  writeField(writeContext, filename, ctx->persistentMemory->line, start, end, field);
}

void writeSubstr(FEC_CONTEXT *ctx, char *filename, int start, int end, FIELD_INFO *field)
{
  writeSubstrToWriter(ctx, ctx->writeContext, filename, start, end, field);
}

// Write a date field by separating the output with dashes
void writeDateField(FEC_CONTEXT *ctx, char *filename, int start, int end, FIELD_INFO *field)
{
  if (end - start != 8)
  {
    printf("Error: Date fields must be exactly 8 chars long, not %d\n", end - start);
  }

  writeSubstrToWriter(ctx, ctx->writeContext, filename, start, start + 4, field);
  writeChar(ctx->writeContext, filename, '-');
  writeSubstrToWriter(ctx, ctx->writeContext, filename, start + 4, start + 6, field);
  writeChar(ctx->writeContext, filename, '-');
  writeSubstrToWriter(ctx, ctx->writeContext, filename, start + 6, start + 8, field);
}

void writeFloatField(FEC_CONTEXT *ctx, char *filename, int start, int end, FIELD_INFO *field)
{
  char *doubleStr;
  char *conversionFloat = ctx->persistentMemory->line->str + start;
  double value = strtod(conversionFloat, &doubleStr);

  if (doubleStr == conversionFloat)
  {
    // Could not convert to a float, write null
    write(ctx->writeContext, filename, "null");
  }

  // Write the value
  writeDouble(ctx->writeContext, filename, value);
}

// Grab a line from the input file.
// Return 0 if there are no lines left.
// If there is a line, decode it into
// ctx->persistentMemory->line.
int grabLine(FEC_CONTEXT *ctx)
{
  ssize_t bytesRead = ctx->getLine(ctx->persistentMemory->rawLine, ctx->file);
  if (bytesRead <= 0)
  {
    return 0;
  }

  // Decode the line
  LINE_INFO info;
  decodeLine(&info, ctx->persistentMemory->rawLine, ctx->persistentMemory->line);
  // Store whether the current line has ascii separators
  // (determines whether we use CSV or ascii28 split line parsing)
  ctx->currentLineHasAscii28 = info.ascii28;
  return 1;
}

char lowercaseTable[256] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
    32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
    48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63,
    64, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o',
    'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', 91, 92, 93, 94, 95,
    96, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o',
    'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', 123, 124, 125, 126, 127,
    128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143,
    144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159,
    160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175,
    176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191,
    192, 193, 194, 195, 196, 197, 198, 199, 200, 201, 202, 203, 204, 205, 206, 207,
    208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219, 220, 221, 222, 223,
    224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239,
    240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255};

void lineToLowerCase(FEC_CONTEXT *ctx)
{
  // Convert the line to lower case
  char *c = ctx->persistentMemory->line->str;
  while (*c)
  {
    *c = lowercaseTable[*c];
    c++;
  }
}

// Check if the line starts with the prefix
int lineStartsWith(FEC_CONTEXT *ctx, const char *prefix, const int prefixLength)
{
  return ctx->persistentMemory->line->n >= prefixLength && strncmp(ctx->persistentMemory->line->str, prefix, prefixLength) == 0;
}

// Return whether the line starts with "/*"
int lineStartsWithLegacyHeader(FEC_CONTEXT *ctx)
{
  return lineStartsWith(ctx, "/*", 2);
}

// Return whether the line starts with "schedule_counts"
int lineStartsWithScheduleCounts(FEC_CONTEXT *ctx)
{
  return lineStartsWith(ctx, "schedule_counts", 15);
}

// Consume whitespace, advancing a position pointer at the same time
void consumeWhitespace(FEC_CONTEXT *ctx, int *position)
{
  while (*position < ctx->persistentMemory->line->n)
  {
    if ((ctx->persistentMemory->line->str[*position] == ' ') || (ctx->persistentMemory->line->str[*position] == '\t'))
    {
      (*position)++;
    }
    else
    {
      break;
    }
  }
}

// Consume characters until the specified character is reached.
// Returns the position of the final character consumed excluding
// trailing whitespace.
int consumeUntil(FEC_CONTEXT *ctx, int *position, char c)
{
  // Store the last non-whitespace character
  int finalNonwhitespace = *position;
  while (*position < ctx->persistentMemory->line->n)
  {
    // Grab the current character
    char current = ctx->persistentMemory->line->str[*position];
    if ((current == c) || (current == 0))
    {
      // If the character is the one we're looking for, break
      break;
    }
    else if ((current != ' ') && (current != '\t') && (current != '\n'))
    {
      // If the character is not whitespace, advance finalNonwhitespace
      finalNonwhitespace = (*position) + 1;
    }
    (*position)++;
  }
  return finalNonwhitespace;
}

void initParseContext(FEC_CONTEXT *ctx, PARSE_CONTEXT *parseContext, FIELD_INFO *fieldInfo)
{
  parseContext->line = ctx->persistentMemory->line;
  parseContext->fieldInfo = fieldInfo;
  parseContext->position = 0;
  parseContext->start = 0;
  parseContext->end = 0;
  parseContext->columnIndex = 0;
}

void readField(FEC_CONTEXT *ctx, PARSE_CONTEXT *parseContext)
{
  // Reset field info
  parseContext->fieldInfo->num_quotes = 0;
  parseContext->fieldInfo->num_commas = 0;

  if (ctx->currentLineHasAscii28)
  {
    readAscii28Field(parseContext);
  }
  else
  {
    readCsvField(parseContext);
  }
}

// Parse a line from a filing, using FEC and form version
// information to map fields to headers and types.
// Return 1 if successful, or 0 if the line is not fully
// specified.
int parseLine(FEC_CONTEXT *ctx, char *filename)
{
  // Parse fields
  PARSE_CONTEXT parseContext;
  FIELD_INFO fieldInfo;
  initParseContext(ctx, &parseContext, &fieldInfo);

  // Log the indices on the line where the form version is specified
  int formStart;
  int formEnd;

  // Iterate through fields
  while (!isParseDone(&parseContext))
  {
    readField(ctx, &parseContext);
    if (parseContext.columnIndex == 0)
    {
      // Set the form version to the first column
      // (with whitespace removed)
      stripWhitespace(&parseContext);
      formStart = parseContext.start;
      formEnd = parseContext.end;
      lookupMappings(ctx, &parseContext, formStart, formEnd);

      // Set filename if null to form type
      if (filename == NULL)
      {
        filename = ctx->formType;
      }
    }
    else
    {
      // If column index is 1, then there are at least two columns
      // and the line is fully specified, so write header info
      if (parseContext.columnIndex == 1)
      {
        // Write header if necessary
        if (getFile(ctx->writeContext, filename) == 1)
        {
          // File is newly opened, write headers
          write(ctx->writeContext, filename, ctx->headers);
          writeNewline(ctx->writeContext, filename);
        }

        // Write form type
        write(ctx->writeContext, filename, ctx->formType);
      }

      // Write delimeter
      writeDelimeter(ctx->writeContext, filename);

      // Get the type of the current field
      if (parseContext.columnIndex < ctx->numFields)
      {
        // Ensure the column index is in bounds
        char type = ctx->types[parseContext.columnIndex];

        // Iterate possible types
        if (type == 's')
        {
          // String
          writeSubstr(ctx, filename, parseContext.start, parseContext.end, parseContext.fieldInfo);
        }
        else if (type == 'd')
        {
          // Date
          writeDateField(ctx, filename, parseContext.start, parseContext.end, parseContext.fieldInfo);
        }
        else if (type == 'f')
        {
          // Float
          writeFloatField(ctx, filename, parseContext.start, parseContext.end, parseContext.fieldInfo);
        }
        else
        {
          // Unknown type
          printf("Unknown type %c\n", type);
          exit(1);
        }
      }
    }

    if (isParseDone(&parseContext))
    {
      break;
    }
    advanceField(&parseContext);
  }

  if (parseContext.columnIndex < 2)
  {
    // Fewer than two fields? The line isn't fully specified
    return 0;
  }

  // Parsing successful
  writeNewline(ctx->writeContext, filename);
  return 1;
}

// Set the FEC context version based on a substring of the current line
void setVersion(FEC_CONTEXT *ctx, int start, int end)
{
  ctx->version = malloc(end - start + 1);
  strncpy(ctx->version, ctx->persistentMemory->line->str + start, end - start);
  // Add null terminator
  ctx->version[end - start] = 0;
  ctx->versionLength = end - start;

  // Calculate whether to use ascii28 or not based on version
  char *dot = strchr(ctx->version, '.');
  int useCommaVersion = 0;
  if (dot != NULL)
  {
    // Check text of left of the dot to get main version
    int dotIndex = (int)(dot - ctx->version);

    for (int i = 0; i < NUM_COMMA_FEC_VERSIONS; i++)
    {
      if (strncmp(ctx->version, COMMA_FEC_VERSIONS[i], dotIndex) == 0)
      {
        useCommaVersion = 1;
        break;
      }
    }
  }

  ctx->useAscii28 = !useCommaVersion;
}

void parseHeader(FEC_CONTEXT *ctx)
{
  // Check if the line starts with "/*"
  if (lineStartsWithLegacyHeader(ctx))
  {
    // Parse legacy header
    int scheduleCounts = 0; // init scheduleCounts to be false
    int firstField = 1;

    // Use a local buffer to store header values
    WRITE_CONTEXT bufferWriteContext;
    initializeLocalWriteContext(&bufferWriteContext, ctx->persistentMemory->bufferLine);

    // Until the line starts with "/*" again, read lines
    while (1)
    {
      if (grabLine(ctx) == 0)
      {
        break;
      }
      if (lineStartsWithLegacyHeader(ctx))
      {
        break;
      }

      // Turn the line into lowercase
      lineToLowerCase(ctx);

      // Check if the line starts with "schedule_counts"
      if (lineStartsWithScheduleCounts(ctx))
      {
        scheduleCounts = 1;
      }
      else
      {
        // Grab key value from "key=value" (strip whitespace)
        int i = 0;
        consumeWhitespace(ctx, &i);
        int keyStart = i;
        int keyEnd = consumeUntil(ctx, &i, '=');
        // Jump over '='
        i++;
        consumeWhitespace(ctx, &i);
        int valueStart = i;
        int valueEnd = consumeUntil(ctx, &i, 0);

        // Gather field metrics for CSV writing
        FIELD_INFO headerField = {.num_quotes = 0, .num_commas = 0};
        for (int i = keyStart; i < keyEnd; i++)
        {
          processFieldChar(ctx->persistentMemory->line->str[i], &headerField);
        }
        FIELD_INFO valueField = {.num_quotes = 0, .num_commas = 0};
        for (int i = valueStart; i < valueEnd; i++)
        {
          processFieldChar(ctx->persistentMemory->line->str[i], &valueField);
        }

        // Write commas as needed (only before fields that aren't first)
        if (!firstField)
        {
          writeDelimeter(ctx->writeContext, HEADER);
          writeDelimeter(&bufferWriteContext, NULL);
        }
        firstField = 0;

        // Write schedule counts prefix if set
        if (scheduleCounts)
        {
          write(ctx->writeContext, HEADER, SCHEDULE_COUNTS);
        }

        // If we match the FEC version column, set the version
        if (strncmp(ctx->persistentMemory->line->str + keyStart, FEC_VERSION_NUMBER, strlen(FEC_VERSION_NUMBER)) == 0)
        {
          setVersion(ctx, valueStart, valueEnd);
        }

        // Write the key/value pair
        writeSubstr(ctx, HEADER, keyStart, keyEnd, &headerField);
        // Write the value to a buffer to be written later
        writeSubstrToWriter(ctx, &bufferWriteContext, NULL, valueStart, valueEnd, &valueField);
      }
    }
    writeNewline(ctx->writeContext, HEADER);
    write(ctx->writeContext, HEADER, bufferWriteContext.localBuffer->str);
    writeNewline(ctx->writeContext, HEADER); // end with newline
  }
  else
  {
    // Not a multiline legacy header — must be using a more recent version

    // Parse fields
    PARSE_CONTEXT parseContext;
    FIELD_INFO fieldInfo;
    initParseContext(ctx, &parseContext, &fieldInfo);

    int isFecSecondColumn = 0;

    // Iterate through fields
    while (!isParseDone(&parseContext))
    {
      readField(ctx, &parseContext);

      if (parseContext.columnIndex == 1)
      {
        // Check if the second column is "FEC"
        if (strncmp(ctx->persistentMemory->line->str + parseContext.start, FEC, strlen(FEC)) == 0)
        {
          isFecSecondColumn = 1;
        }
        else
        {
          // If not, the second column is the version
          setVersion(ctx, parseContext.start, parseContext.end);

          // Parse the header now that version is known
          parseLine(ctx, HEADER);
        }
      }
      if (parseContext.columnIndex == 2 && isFecSecondColumn)
      {
        // Set the version
        setVersion(ctx, parseContext.start, parseContext.end);

        // Parse the header now that version is known
        parseLine(ctx, HEADER);
        return;
      }

      if (isParseDone(&parseContext))
      {
        break;
      }
      advanceField(&parseContext);
    }
  }
}

int parseFec(FEC_CONTEXT *ctx)
{
  if (grabLine(ctx) == 0)
  {
    return 0;
  }

  // Parse the header
  parseHeader(ctx);

  // Write the entire file out as tabular data
  // TEMP code for perf testing
  while (1)
  {
    if (grabLine(ctx) == 0)
    {
      break;
    }

    parseLine(ctx, NULL);
  }

  //   int position = 0;
  //   int start = 0;
  //   int end = 0;
  //   int first = 1;

  //   while (position < ctx->persistentMemory->line->n && ctx->persistentMemory->line->str[position] != 0)
  //   {
  //     if (!first)
  //     {
  //       // Only write commas before fields that aren't first
  //       writeDelimeter(ctx->writeContext, "data");
  //     }
  //     first = 0;

  //     FIELD_INFO field = {.num_quotes = 0, .num_commas = 0};
  //     readAscii28Field(ctx->persistentMemory->line, &position, &start, &end, &field);
  //     writeSubstr(ctx, "data", start, end, &field);
  //     position++;
  //   }
  //   writeNewline(ctx->writeContext, "data");
  // }
  return 1;
}