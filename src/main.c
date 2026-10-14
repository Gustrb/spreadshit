#include <inttypes.h>
#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

#define BUFFERED_READER_SIZE 1024
#define EXPRESSION_POOL_SIZE 1024
#define EXPRESSION_MAX_SIZE  1024

#define DOCUMENT_MAX_HEADERS 256
#define DOCUMENT_MAX_ROWS    1024
#define HEADER_MAX_CHARS     16

#define INTEGER_MAX_CHARS    21
#define STRING_MAX_CHARS     32
#define INTEGER_MAX_VALUE    UINT32_MAX

// TODO: Allow this to be configured
#define DOCUMENT_ROW_SEPARATOR  ','
#define DOCUMENT_LINE_SEPARATOR '\n'

#define BOOL_LITERAL_TRUE "true"
#define BOOL_LITERAL_FALSE "false"

#define EXPRESSION_MARKER '='

#define MIN(a, b) (a) < (b) ? (a) : (b)
#define MAX(a, b) (a) > (b) ? (a) : (b)

typedef struct buffered_reader_t {
  FILE *fptr;
  char buffer[BUFFERED_READER_SIZE];

  size_t len;
  size_t offset;
} buffered_reader_t;

typedef struct header_t {
  char buff[HEADER_MAX_CHARS];
  uint8_t header_len;
} header_t;

typedef struct {
  char buffer[STRING_MAX_CHARS];
  uint8_t len;
} string_t;

typedef enum {
  EXPRESSION_TYPE_CELL,
  EXPRESSION_TYPE_LITERAL,
  EXPRESSION_TYPE_ADD,
  EXPRESSION_TYPE_SUB,
  EXPRESSION_TYPE_MUL,
  EXPRESSION_TYPE_DIV,
} expression_type_t;

typedef struct expression_t {
  expression_type_t type;
  union {
    uint64_t literal;
    struct { uint16_t col; uint16_t row; } cel;
    struct { uint16_t left; uint16_t right; } binary;
  } as;
} expression_t;

typedef enum {
  CELL_TYPE_EMPTY,
  CELL_TYPE_INTEGER,
  CELL_TYPE_BOOLEAN,
  CELL_TYPE_STRING,
  CELL_TYPE_EXPRESSION,
} cell_type_t;

typedef struct cell_t {
  cell_type_t type;
  union {
    uint64_t integer;
    uint8_t  boolean;
    string_t string;
    uint16_t formula;
  } as;
} cell_t;

typedef struct document_t {
  header_t headers[DOCUMENT_MAX_HEADERS];
  uint16_t headers_len;

  cell_t rows[DOCUMENT_MAX_ROWS];
  uint16_t rows_len;
} document_t;

typedef uint32_t error_code_t;

typedef enum { STATE_BLANK, STATE_NUMBER, STATE_STRING, STATE_EXPRESSION } document_parser_state_t;

#define ERROR_PARSING_HEADERS            (1 << 10) + 1
#define ERROR_TOO_MANY_HEADERS           (1 << 10) + 2
#define ERROR_HEADER_TOO_LARGE           (1 << 10) + 3
#define ERROR_STRING_TOO_LARGE           (1 << 10) + 4
#define ERROR_INTEGER_TOO_LARGE          (1 << 10) + 5
#define ERROR_INTEGER_FAILED_TO_PARSE    (1 << 10) + 6
#define ERROR_EXPRESSION_TOO_LARGE       (1 << 10) + 7
#define ERROR_EXPRESSION_UNEXPECTED_CHAR (1 << 10) + 8
#define ERROR_EXPECTED_INTEGER           (1 << 10) + 9
#define ERROR_EXPRESSION_EXPECTED_RPAREN (1 << 10) + 10

uint8_t buffered_reader_getc(buffered_reader_t *reader, char *c);

error_code_t string_parse_to_integer(string_t *string, uint64_t *val);
error_code_t string_parse_to_bool(string_t *string, cell_t *cell);
error_code_t document_parse_headers_from_buffer(document_t *document, buffered_reader_t *reader);
error_code_t document_parse_rows_from_buffer(document_t *document, buffered_reader_t *reader);
error_code_t cell_finalize(cell_t *cell, document_parser_state_t state, char *expression_buff, size_t *expression_buffer_len);
error_code_t expression_parse_into_cell(cell_t *cell, const char *buffer, size_t len);

static inline uint8_t is_alphabetic(char c);
static inline uint8_t is_numeric(char c);
static inline uint8_t is_space(char c);
static inline uint8_t digit(char c);

static void document_export(document_t *document);
static void header_export(header_t *header);
static void row_export(document_t *document, cell_t *row);

static error_code_t expression_export(document_t *document, expression_t *expression);
static error_code_t binary_expression_eval(document_t *document, expression_t *expression, uint64_t *val);
static error_code_t cell_expression_eval(document_t *document, expression_t *expression, uint64_t *val);

static expression_t expression_pool[EXPRESSION_POOL_SIZE] = {0};
static uint16_t expression_pool_length = 0;

int main(void)
{
  document_t document = {0};
  buffered_reader_t reader = {0};
  reader.fptr = stdin;

  error_code_t error_code = document_parse_headers_from_buffer(&document, &reader);
  if (error_code != 0)
  {
    // TODO: give proper diagnostics...
    return 1;
  }

  error_code = document_parse_rows_from_buffer(&document, &reader);
  if (error_code != 0)
  {
    // TODO: give proper diagnostics...
    return 1;
  }
  
  document_export(&document);
  return 0;
}

error_code_t document_parse_headers_from_buffer(document_t *document, buffered_reader_t *reader)
{
  assert(document != NULL);
  assert(reader != NULL);

  char current_char;

  size_t header_num = 0;

  while (buffered_reader_getc(reader, &current_char))
  {
    assert(current_char != 0);

    if (current_char == ' ')
    {
      continue;
    }

    if (current_char == DOCUMENT_LINE_SEPARATOR)
    {
      header_num++;
      break;
    }

    if (current_char == DOCUMENT_ROW_SEPARATOR)
    {
      header_num++;
      continue;
    }

    if (document->headers[header_num].header_len >= HEADER_MAX_CHARS)
    {
      return ERROR_HEADER_TOO_LARGE;
    }

    document->headers[header_num].buff[document->headers[header_num].header_len++] = current_char;
  }

  if (document->headers[header_num].header_len > 0)
  {
    header_num++;
  }

  document->headers_len = header_num;

  return 0;  
}


error_code_t document_parse_rows_from_buffer(document_t *document, buffered_reader_t *reader)
{
  assert(document != NULL);
  assert(reader != NULL);

  char current_char;
  size_t row_number = 0;

  char expression_buffer[EXPRESSION_MAX_SIZE] = {0};
  size_t expression_buffer_len = 0;

  document_parser_state_t state = STATE_BLANK;

loop:
  while (buffered_reader_getc(reader, &current_char))
  {
    assert(current_char != 0);
    cell_t *current_cell = &document->rows[row_number];

    error_code_t err;

    switch (state)
    {
    case STATE_BLANK:
    {
      if (current_char == DOCUMENT_ROW_SEPARATOR || current_char == DOCUMENT_LINE_SEPARATOR)
      {
        row_number++;
      }
      else if (is_alphabetic(current_char))
      {
        assert(current_cell->as.string.len == 0);

        current_cell->type = CELL_TYPE_STRING;
        current_cell->as.string.buffer[current_cell->as.string.len++] = current_char;
        state = STATE_STRING;
      }
      else if (is_numeric(current_char))
      {
        assert(current_cell->as.string.len == 0);

        // Yes, this is weird, we are writing into a union in the wrong place.
        // but this is going to be useful for parsing the number later...
        current_cell->type = CELL_TYPE_INTEGER;
        current_cell->as.string.buffer[current_cell->as.string.len++] = current_char;
        state = STATE_NUMBER;
      }
      else if (current_char == EXPRESSION_MARKER)
      {
        current_cell->type = CELL_TYPE_EXPRESSION;
        state = STATE_EXPRESSION;
      }
      goto loop;
    };

    case STATE_STRING:
    {
      if (current_char == DOCUMENT_ROW_SEPARATOR || current_char == DOCUMENT_LINE_SEPARATOR)
      {
        if ((err = cell_finalize(current_cell, state, expression_buffer, &expression_buffer_len)) != 0)
        {
          return err;
        }
        row_number++;
        state = STATE_BLANK;
        goto loop;
      }
      else if (current_cell->as.string.len >= STRING_MAX_CHARS)
      {
        return ERROR_STRING_TOO_LARGE;
      }

      current_cell->as.string.buffer[current_cell->as.string.len++] = current_char;
      goto loop;
    };

    case STATE_NUMBER:
    {
      if (current_char == DOCUMENT_ROW_SEPARATOR || current_char == DOCUMENT_LINE_SEPARATOR)
      {
        if ((err = cell_finalize(current_cell, state, expression_buffer, &expression_buffer_len)) != 0)
        {
          return err;
        }
        row_number++;
        state = STATE_BLANK;
        goto loop;
      }
      else if (current_cell->as.string.len >= INTEGER_MAX_CHARS)
      {
        return ERROR_INTEGER_TOO_LARGE;
      }

      current_cell->as.string.buffer[current_cell->as.string.len++] = current_char;
      goto loop;
    };

    case STATE_EXPRESSION:
    {
      if (current_char == DOCUMENT_LINE_SEPARATOR || current_char == DOCUMENT_ROW_SEPARATOR)
      {
        if ((err = cell_finalize(current_cell, state, expression_buffer, &expression_buffer_len)) != 0)
        {
          return err;
        }
        state = STATE_BLANK;
        row_number++;

        goto loop;
      }

      if (expression_buffer_len >= EXPRESSION_MAX_SIZE)
      {
        return ERROR_EXPRESSION_TOO_LARGE;
      }

      expression_buffer[expression_buffer_len++] = current_char;
      goto loop;
    };
    }
  }

  if (state != STATE_BLANK)
  {
    error_code_t err = cell_finalize(&document->rows[row_number], state, expression_buffer, &expression_buffer_len);
    if (err != 0)
    {
      return err;
    }

    row_number++;
  }

  document->rows_len = row_number;

  return 0;
}

error_code_t cell_finalize(cell_t *cell, document_parser_state_t state, char *expression_buff, size_t *expression_buffer_len)
{
  error_code_t err = 0;
  switch (state)
  {
  case STATE_STRING:
  {
    err = string_parse_to_bool(&cell->as.string, cell);
  }; break;
  case STATE_NUMBER:
  {
    cell->type = CELL_TYPE_INTEGER;
    err = string_parse_to_integer(&cell->as.string, &cell->as.integer);
  }; break;
  case STATE_EXPRESSION:
  {
    err = expression_parse_into_cell(cell, expression_buff, *expression_buffer_len);
    *expression_buffer_len = 0;
  }; break;
  default: {} break;
  }
  return err;
}

error_code_t string_parse_to_integer(string_t *string, uint64_t *val)
{
  assert(string->len < INTEGER_MAX_CHARS);
  string->buffer[string->len] = '\0';

  char *end;
  errno = 0;

  uintmax_t value = strtoumax(string->buffer, &end, 10);
  if (errno == ERANGE)
  {
    return ERROR_INTEGER_TOO_LARGE;
  }

  if (*end != '\0')
  {
    return ERROR_INTEGER_FAILED_TO_PARSE;
  }

  *val = (uint64_t)value;
  return 0;
}

error_code_t string_parse_to_bool(string_t *string, cell_t *cell)
{
  size_t literal_true_len = strlen(BOOL_LITERAL_TRUE);
  size_t literal_false_len = strlen(BOOL_LITERAL_FALSE);

  if (string->len == literal_true_len && strncmp(string->buffer, BOOL_LITERAL_TRUE, literal_true_len) == 0)
  {
    cell->type = CELL_TYPE_BOOLEAN;
    cell->as.boolean = 1;
    return 0;
  }

  if (string->len == literal_false_len && strncmp(string->buffer, BOOL_LITERAL_FALSE, literal_false_len) == 0)
  {
    cell->type = CELL_TYPE_BOOLEAN;
    cell->as.boolean = 0;
    return 0;
  }

  cell->type = CELL_TYPE_STRING;

  return 0;
}

typedef struct {
  const char *buffer;
  size_t len;
  size_t pos;
} expression_parser_t;

// expr := term (('+' | '-') term)*
// term := factor (('*' | '/') factor)*
// factor := NUMBER | CELLREF | '(' expr ')'

static error_code_t parse_expression(expression_parser_t *p, uint16_t *out);
static error_code_t parse_term(expression_parser_t *p, uint16_t *out);
static error_code_t parse_factor(expression_parser_t *p, uint16_t *out);

void expression_skip_spaces(expression_parser_t *p);
char expression_peek(expression_parser_t *p);
char expression_advance(expression_parser_t *p);

static uint16_t expression_pool_push(expression_t e)
{
  // TODO: this shouldn't fail like this
  assert(expression_pool_length < EXPRESSION_POOL_SIZE);

  expression_pool[expression_pool_length] = e;
  return expression_pool_length++;
}

error_code_t expression_parse_into_cell(cell_t *cell, const char *buffer, size_t len)
{
  assert(cell != NULL);
  assert(buffer != NULL);

  expression_parser_t parser = {.buffer=buffer, .len=len, .pos=0};

  cell->type = CELL_TYPE_EXPRESSION;
  error_code_t err = 0;

  if ((err = parse_expression(&parser, &cell->as.formula)) != 0)
  {
    return err;
  }

  return 0;
}

static error_code_t parse_expression(expression_parser_t *p, uint16_t *out)
{
  error_code_t err = parse_term(p, out);
  if (err) return err;  

  while (1)
  {
    expression_skip_spaces(p);
    char c = expression_peek(p);

    if (c != '+' && c != '-') return 0;
    p->pos++;

    uint16_t rhs;
    err = parse_term(p, &rhs);
    if (err) return err;

    expression_t e = {.type = c == '+' ? EXPRESSION_TYPE_ADD : EXPRESSION_TYPE_SUB };
    e.as.binary.left = *out;
    e.as.binary.right = rhs;
    *out = expression_pool_push(e);
  }
  return 0;
}

static error_code_t parse_term(expression_parser_t *p, uint16_t *out)
{
  error_code_t err = parse_factor(p, out);
  if (err) return err;  

  while (1)
  {
    expression_skip_spaces(p);
    char c = expression_peek(p);

    if (c != '*' &&  c != '/') return 0;
    p->pos++;

    uint16_t rhs;
    err = parse_factor(p, &rhs);
    if (err) return err;

    expression_t e = {.type = c == '*' ? EXPRESSION_TYPE_MUL : EXPRESSION_TYPE_DIV };
    e.as.binary.left = *out;
    e.as.binary.right = rhs;
    *out = expression_pool_push(e);
  }
}

static error_code_t parse_factor(expression_parser_t *p, uint16_t *out)
{
  expression_skip_spaces(p);
  char c = expression_peek(p);

  error_code_t err;

  if (is_numeric(c))
  {
    // TODO: have a function to parse literals
    string_t string = {0};
    while (is_numeric(c))
    {
      if (string.len >= INTEGER_MAX_CHARS) return ERROR_INTEGER_TOO_LARGE;

      string.buffer[string.len++] = c;
      c = expression_advance(p);
    }

    uint64_t v = 0;
    if ((err = string_parse_to_integer(&string, &v)) != 0)
    {
      return err;
    }

    expression_t e;
    e.type = EXPRESSION_TYPE_LITERAL;
    e.as.literal = v;
    *out = expression_pool_push(e);
    return 0;
  }
  else if (is_alphabetic(c))
  {
    char column_identifier = c;
    c = expression_advance(p);
    if (!is_numeric(c)) return ERROR_EXPECTED_INTEGER;

    // TODO: have a function to parse literals
    string_t string = {0};
    while (is_numeric(c))
    {
      if (string.len >= INTEGER_MAX_CHARS) return ERROR_INTEGER_TOO_LARGE;

      string.buffer[string.len++] = c;
      c = expression_advance(p);
    }

    uint64_t v = 0;
    if ((err = string_parse_to_integer(&string, &v)) != 0)
    {
      return err;
    }

    expression_t e;
    e.type = EXPRESSION_TYPE_CELL;

    e.as.cel.col = column_identifier - 'A';
    e.as.cel.row = v;

    *out = expression_pool_push(e);
    return 0;
  }
  else if (c == '(')
  {
    p->pos++;
    err = parse_expression(p, out);
    if (err) return err;
    expression_skip_spaces(p);
    if (expression_peek(p) != ')') return ERROR_EXPRESSION_EXPECTED_RPAREN;
    p->pos++;
    return 0;
  }

  return ERROR_EXPRESSION_UNEXPECTED_CHAR;
}


void expression_skip_spaces(expression_parser_t *p)
{
  while (p->pos < p->len && is_space(p->buffer[p->pos])) p->pos++;
}

char expression_peek(expression_parser_t *p)
{
  if (p->pos >= p->len) return 0;
  return p->buffer[p->pos];
}

char expression_advance(expression_parser_t *p)
{
  p->pos++;
  return expression_peek(p);
}

static void document_export(document_t *document)
{
  assert(document != NULL);

  for (uint16_t l = 0; l < document->headers_len; l++)
  {
    header_export(&document->headers[l]);
    if (l < document->headers_len-1)
      printf(",");
  }

  printf("\n");

  size_t col = 0;
  for (uint16_t l = 0; l < document->rows_len; l++)
  {
    if (col == document->headers_len)
    {
      col = 0;
      printf("\n");
    }

    row_export(document, &document->rows[l]);
    col++;

    if (col < document->headers_len) printf(",");
  }
  printf("\n");
}

static void header_export(header_t *header)
{
  assert(header != NULL);
  assert(header->buff != NULL);
  assert(header->header_len > 0);

  printf("%.*s", header->header_len, header->buff);
}

static void row_export(document_t *document, cell_t *row)
{
  switch (row->type)
  {
  case CELL_TYPE_STRING:
  {
    printf("%.*s", row->as.string.len, row->as.string.buffer);
  }; return;
  case CELL_TYPE_INTEGER:
  {
    printf("%ld", row->as.integer);
  }; return;
  case CELL_TYPE_BOOLEAN:
  {
    printf("%s", row->as.boolean ? "true" : "false");
  }; return;
  case CELL_TYPE_EXPRESSION:
  {
    error_code_t err = expression_export(document, &expression_pool[row->as.formula]);
    if (err != 0)
    {
      printf("Failure evaluating expression\n");
      return;
    }
    // TODO: implement this... We need to eval the expression
  }; return;
  case CELL_TYPE_EMPTY: {}; return;
  } 
}

static error_code_t expression_eval(document_t *document, expression_t *expression, uint64_t *val);

static error_code_t expression_export(document_t *document, expression_t *expression)
{
  uint64_t val;
  error_code_t err = expression_eval(document, expression, &val);
  if (err != 0)
  {
    printf("[ERROR]: %d", err);
    return err;
  }

  printf("%lu", val);

  return 0;
}

static error_code_t expression_eval(document_t *document, expression_t *expression, uint64_t *val)
{  
  error_code_t err = 0;

  switch (expression->type) {
  case EXPRESSION_TYPE_ADD:
  case EXPRESSION_TYPE_DIV:
  case EXPRESSION_TYPE_MUL:
  case EXPRESSION_TYPE_SUB:
  {
    if ((err = binary_expression_eval(document, expression, val)) != 0)
    {
      return err;
    }

  }; break;
  case EXPRESSION_TYPE_LITERAL:
  {
    *val = expression->as.literal;
  }; break;
  case EXPRESSION_TYPE_CELL:
  {
    if ((err = cell_expression_eval(document, expression, val)) != 0)
    {
      return err;
    }
  }; break;
  }

  return 0;
}

static error_code_t binary_expression_eval(document_t *document, expression_t *expression, uint64_t *val)
{
  error_code_t err = 0;  

  uint64_t lhs;
  if ((err = expression_eval(document, &expression_pool[expression->as.binary.left], &lhs)) != 0)
  {
    return err;
  }

  uint64_t rhs;
  if ((err = expression_eval(document, &expression_pool[expression->as.binary.right], &rhs)) != 0)
  {
    return err;
  }

  switch (expression->type)
  {
  case EXPRESSION_TYPE_ADD:{ *val = lhs + rhs; }; break;
  case EXPRESSION_TYPE_SUB:{ *val = lhs - rhs; }; break;
  case EXPRESSION_TYPE_MUL:{ *val = lhs * rhs; }; break;
  case EXPRESSION_TYPE_DIV:{ *val = lhs / rhs; }; break;
  default: {}; break;
  }

  return err;
}

static error_code_t cell_expression_eval(document_t *document, expression_t *expression, uint64_t *val)
{
  // Note: this does not handle infinite recursive handling, implement it later
  // document->header_number is how wide the table is, so to identify the element at row 1, colum 3
  // we have to walk 1*document->header_number+3 elements to get the cell
  size_t idx = (expression->as.cel.row -1)* document->headers_len + expression->as.cel.col;
  cell_t *refcell = &document->rows[idx ];

  switch (refcell->type)
  {
  case CELL_TYPE_BOOLEAN: { /* TODO: return type error */ }; break;
  case CELL_TYPE_EMPTY: { /* TODO: return type error */ }; break;
  case CELL_TYPE_STRING: { /* TODO: return type error */ }; break;
  case CELL_TYPE_INTEGER: { *val = refcell->as.integer; }; break;
  case CELL_TYPE_EXPRESSION: {
      error_code_t err = 0;
      if ((err = expression_eval(document, &expression_pool[refcell->as.formula], val)) != 0)
      {
        return err;
      }
  }; break;
  }

  return 0;
}

uint8_t buffered_reader_getc(buffered_reader_t *reader, char *c)
{
  if (reader->offset == reader->len)
  {
    reader->offset = 0;

    reader->len = fread(
      reader->buffer,
      sizeof(char),
      BUFFERED_READER_SIZE,
      reader->fptr
    );

    if (reader->len == 0) return 0;
  }  

  *c = reader->buffer[reader->offset++];
  return 1;
}

static inline uint8_t is_alphabetic(char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static inline uint8_t is_numeric(char c)
{
  return c >= '0' && c <= '9';
}

static inline uint8_t is_space(char c)
{
  return c == ' ' || c == '\t';
}

static inline uint8_t digit(char c)
{
  assert(c >= 48);
  return c - 48;
}
