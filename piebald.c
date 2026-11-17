/*
 * piebald.c -- interpreter and C compiler for the Piebald language.
 *
 *   piebald [options] program.pie
 *
 *   -c      compile: write an equivalent C program to stdout instead of running
 *   -O0     disable optimisations (dead-wire elimination, row compaction)
 *   -s      print execution statistics to stderr when the program ends
 *   -h      help
 *
 * See README.md for the language definition.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#endif

#define TAPE 65536u /* cells on the (circular) tape; must be a power of two */
#define MASK (TAPE - 1u)

/* ------------------------------------------------------------------------ */
/* Program representation                                                   */

enum {
  OP_SET,    /* 0..7  cell |= arg            */
  OP_ZERO,   /* !     cell  = 0              */
  OP_NOT,    /* ~     cell  = ~cell          */
  OP_SHL,    /* <     cell <<= 1             */
  OP_SHR,    /* >     cell >>= 1             */
  OP_OR,     /* |     cell |= right          */
  OP_AND,    /* &     cell &= right          */
  OP_XOR,    /* ^     cell ^= right          */
  OP_FROMR,  /* /     cell  = right  (also {) */
  OP_FROML,  /* \     cell  = left   (also }) */
  OP_IN,     /* ,     cell  = getchar        */
  OP_OUT,    /* .     putchar(cell)          */
  OP_NUM,    /* @     print cell as decimal  */
  OP_SKIPIN  /* ,  on a dead wire: consume one input byte only */
};

enum { BR_NONE, BR_IFZ /* ? */, BR_IFNZ /* : */ };

typedef struct {
  uint32_t col;
  uint8_t kind;
  uint8_t arg;
} Op;

typedef struct {
  uint32_t first, nops; /* slice of Prog.ops                          */
  uint8_t br;           /* BR_*                                       */
  uint32_t brcol;       /* column of the ? or :                       */
  uint32_t target;      /* row to jump to when the branch is taken    */
  int32_t slide;        /* net ) minus ( on this row                  */
  int line;             /* 1-based source line                        */
} Row;

typedef struct {
  const char *file;
  Row *rows;
  uint32_t nrows, caprows;
  Op *ops;
  uint32_t nops, capops;
  uint32_t width;  /* 1 + rightmost column holding an instruction */
  int slides;      /* program uses ( or ) anywhere                */
} Prog;

static void die(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  exit(2);
}

static void *xrealloc(void *p, size_t n)
{
  p = realloc(p, n ? n : 1);
  if (!p) die("piebald: out of memory");
  return p;
}

static int writes(int kind) { return kind != OP_OUT && kind != OP_NUM && kind != OP_SKIPIN; }

/* Which neighbour (if any) an op reads: +1 right, -1 left, 0 none. */
static int neighbour(int kind)
{
  switch (kind) {
  case OP_OR: case OP_AND: case OP_XOR: case OP_FROMR: return 1;
  case OP_FROML: return -1;
  default: return 0;
  }
}

/* ------------------------------------------------------------------------ */
/* Parsing                                                                  */

static char *slurp(const char *file, size_t *len)
{
  FILE *f = fopen(file, "rb");
  char *buf = NULL;
  size_t n = 0, cap = 0, got;
  if (!f) die("piebald: cannot open '%s'", file);
  do {
    if (cap - n < 65536) buf = xrealloc(buf, cap = cap * 2 + 65536);
    got = fread(buf + n, 1, cap - n, f);
    n += got;
  } while (got);
  fclose(f);
  *len = n;
  return buf;
}

static void parse(Prog *p, const char *file)
{
  size_t len, pos = 0;
  char *src = slurp(file, &len);
  int line = 0;
  /* open[c]: most recent unmatched '?' row in column c; link[r]: the one before it */
  int32_t *open = xrealloc(NULL, TAPE * sizeof *open);
  int32_t *link = NULL;
  uint32_t c;

  memset(p, 0, sizeof *p);
  p->file = file;
  for (c = 0; c < TAPE; c++) open[c] = -1;

  while (pos < len) {
    size_t start = pos, end;
    Row row;
    int nbr = 0;

    while (pos < len && src[pos] != '\n') pos++;
    end = pos++;
    line++;
    if (end > start && src[end - 1] == '\r') end--;
    for (c = 0; start + c < end; c++)
      if (src[start + c] == '#') { end = start + c; break; }
    if (end == start) continue; /* blank and comment-only lines are not rows */
    if (end - start > TAPE) die("%s:%d: line is wider than the tape (%u cells)", file, line, TAPE);

    memset(&row, 0, sizeof row);
    row.first = p->nops;
    row.line = line;
    for (c = 0; start + c < end; c++) {
      char ch = src[start + c];
      Op op;
      op.col = c;
      op.arg = 0;
      switch (ch) {
      case '0': case '1': case '2': case '3':
      case '4': case '5': case '6': case '7':
        op.kind = OP_SET; op.arg = (uint8_t)(1u << (ch - '0')); break;
      case '!': op.kind = OP_ZERO; break;
      case '~': op.kind = OP_NOT; break;
      case '<': op.kind = OP_SHL; break;
      case '>': op.kind = OP_SHR; break;
      case '|': op.kind = OP_OR; break;
      case '&': op.kind = OP_AND; break;
      case '^': op.kind = OP_XOR; break;
      case '/': case '{': op.kind = OP_FROMR; break;  /* { } are the 2012 spellings */
      case '\\': case '}': op.kind = OP_FROML; break;
      case ',': op.kind = OP_IN; break;
      case '.': op.kind = OP_OUT; break;
      case '@': op.kind = OP_NUM; break;
      case '(': row.slide--; p->slides = 1; continue;
      case ')': row.slide++; p->slides = 1; continue;
      case '?': case ':':
        if (nbr++) die("%s:%d:%u: a row may contain only one '?' or ':'", file, line, c + 1);
        row.br = ch == '?' ? BR_IFZ : BR_IFNZ;
        row.brcol = c;
        if (c + 1 > p->width) p->width = c + 1;
        continue;
      case '\t':
        die("%s:%d:%u: tab character (columns are significant; use spaces)", file, line, c + 1);
      default:
        continue; /* everything else is a no-op ("insulation") */
      }
      if (p->nops == p->capops) p->ops = xrealloc(p->ops, (p->capops = p->capops * 2 + 1024) * sizeof *p->ops);
      p->ops[p->nops++] = op;
      if (c + 1 > p->width) p->width = c + 1;
    }
    row.nops = p->nops - row.first;

    if (p->nrows == p->caprows) {
      p->caprows = p->caprows * 2 + 256;
      p->rows = xrealloc(p->rows, p->caprows * sizeof *p->rows);
      link = xrealloc(link, p->caprows * sizeof *link);
    }
    if (row.br == BR_IFZ) {
      link[p->nrows] = open[row.brcol];
      open[row.brcol] = (int32_t)p->nrows;
    } else if (row.br == BR_IFNZ) {
      int32_t s = open[row.brcol];
      if (s < 0) die("%s:%d:%u: ':' has no matching '?' above it in this column", file, line, row.brcol + 1);
      p->rows[s].target = p->nrows + 1; /* ? skips to just after the :  */
      row.target = (uint32_t)s;          /* : loops back to the ? row     */
      open[row.brcol] = link[s];
    }
    p->rows[p->nrows++] = row;
  }

  for (c = 0; c < TAPE; c++)
    if (open[c] >= 0)
      die("%s:%d:%u: '?' has no matching ':' below it in this column", file, p->rows[open[c]].line, c + 1);

  free(open);
  free(link);
  free(src);
}

/* ------------------------------------------------------------------------ */
/* Optimisation                                                             */

/*
 * Dead-wire elimination.  A wire (column) is live if its value can reach the
 * outside world: it is printed (. @), tested (? :), or read by an instruction
 * on another live wire.  Instructions writing to dead wires are deleted.
 * Only valid when the program never slides, because then column == cell.
 */
static void eliminate_dead_wires(Prog *p)
{
  uint8_t *live = xrealloc(NULL, TAPE), *rd = xrealloc(NULL, TAPE);
  uint32_t *work = xrealloc(NULL, TAPE * sizeof *work), nwork = 0, i, r, n;

  memset(live, 0, TAPE);
  memset(rd, 0, TAPE); /* bit0: some op on this wire reads right, bit1: reads left */
  for (i = 0; i < p->nops; i++) {
    int k = p->ops[i].kind, nb = neighbour(k);
    uint32_t c = p->ops[i].col;
    if (nb > 0) rd[c] |= 1;
    if (nb < 0) rd[c] |= 2;
    if ((k == OP_OUT || k == OP_NUM) && !live[c]) live[c] = 1, work[nwork++] = c;
  }
  for (r = 0; r < p->nrows; r++) {
    uint32_t c = p->rows[r].brcol;
    if (p->rows[r].br && !live[c]) live[c] = 1, work[nwork++] = c;
  }
  while (nwork) {
    uint32_t c = work[--nwork];
    if ((rd[c] & 1) && !live[n = (c + 1) & MASK]) live[n] = 1, work[nwork++] = n;
    if ((rd[c] & 2) && !live[n = (c - 1) & MASK]) live[n] = 1, work[nwork++] = n;
  }

  /* compact the op list row by row */
  n = 0;
  for (r = 0; r < p->nrows; r++) {
    Row *R = &p->rows[r];
    uint32_t first = n;
    for (i = R->first; i < R->first + R->nops; i++) {
      Op op = p->ops[i];
      if (!live[op.col]) {
        if (op.kind != OP_IN) continue;
        op.kind = OP_SKIPIN;
      }
      p->ops[n++] = op;
    }
    R->first = first;
    R->nops = n - first;
  }
  p->nops = n;
  free(live);
  free(rd);
  free(work);
}

/* Drop rows that do nothing at all, retargeting jumps. */
static void compact_rows(Prog *p)
{
  uint32_t *map = xrealloc(NULL, (p->nrows + 1) * sizeof *map), r, n = 0;
  for (r = 0; r < p->nrows; r++) {
    const Row *R = &p->rows[r];
    map[r] = n;
    if (R->nops || R->br || R->slide) n++;
  }
  map[p->nrows] = n;
  n = 0;
  for (r = 0; r < p->nrows; r++) {
    Row R = p->rows[r];
    if (!(R.nops || R.br || R.slide)) continue;
    if (R.br) R.target = map[R.target];
    p->rows[n++] = R;
  }
  p->nrows = n;
  free(map);
}

/* ------------------------------------------------------------------------ */
/* I/O                                                                      */

static char obuf[1 << 16];
static size_t olen;
static int stdin_tty, stdout_tty;

static void oflush(void)
{
  if (olen) fwrite(obuf, 1, olen, stdout);
  olen = 0;
  fflush(stdout);
}

static void oputc(uint8_t ch)
{
  obuf[olen++] = (char)ch;
  if (olen == sizeof obuf || (stdout_tty && ch == '\n')) oflush();
}

static void onum(uint8_t v)
{
  if (v >= 100) oputc((uint8_t)('0' + v / 100));
  if (v >= 10) oputc((uint8_t)('0' + v / 10 % 10));
  oputc((uint8_t)('0' + v % 10));
}

static uint8_t iget(void)
{
  int ch;
  if (stdin_tty) oflush();
  ch = getchar();
  return ch == EOF ? 0 : (uint8_t)ch; /* end of input reads as 0 */
}

/* ------------------------------------------------------------------------ */
/* Lowering to a flat stream of micro-ops                                   */

/*
 * Every row executes "simultaneously": each instruction sees the cells as they
 * were before the row started.  Rather than double-buffering every row, the
 * instructions of a row are ordered so that each cell is read before it is
 * overwritten (Kahn's algorithm on the read-before-write graph).  Only a true
 * cycle, such as the swap `/\`, needs a temporary.  The ? or : test can be done
 * after the row, because no instruction on the row can write the branch wire.
 */
enum {
  U_SET, U_ZERO, U_NOT, U_SHL, U_SHR, U_OR, U_AND, U_XOR, U_MOV,
  U_IN, U_OUT, U_NUM, U_SKIPIN,
  U_TEMP,   /* tmp[t] = what op `sub` would write to cell a   */
  U_COMMIT, /* cell a = tmp[t]                                 */
  U_JZ,     /* if cell a == 0 goto t                           */
  U_JNZ,    /* if cell a != 0 goto t                           */
  U_TEST,   /* flag = branch `sub` on cell a would be taken    */
  U_SLIDE,  /* base += (int32_t)t                              */
  U_JF,     /* if flag goto t                                  */
  U_END
};

typedef struct {
  const void *h;    /* handler address, filled in by the interpreter      */
  uint32_t a, n, t; /* cell, neighbour cell, temp slot / jump target / slide */
  uint8_t k, sub, arg;
} UOp;

typedef struct {
  UOp *code;
  uint32_t n, cap;
} Code;

static UOp *emit(Code *c, int k, uint32_t a)
{
  UOp *u;
  if (c->n == c->cap) c->code = xrealloc(c->code, (c->cap = c->cap * 2 + 1024) * sizeof *c->code);
  u = &c->code[c->n++];
  memset(u, 0, sizeof *u);
  u->k = (uint8_t)k;
  u->a = a;
  return u;
}

static void emit_op(Code *c, const Op *o, int temp, uint32_t slot)
{
  static const uint8_t direct[] = {
    [OP_SET] = U_SET, [OP_ZERO] = U_ZERO, [OP_NOT] = U_NOT, [OP_SHL] = U_SHL,
    [OP_SHR] = U_SHR, [OP_OR] = U_OR, [OP_AND] = U_AND, [OP_XOR] = U_XOR,
    [OP_FROMR] = U_MOV, [OP_FROML] = U_MOV, [OP_IN] = U_IN, [OP_OUT] = U_OUT,
    [OP_NUM] = U_NUM, [OP_SKIPIN] = U_SKIPIN
  };
  UOp *u = emit(c, temp ? U_TEMP : direct[o->kind], o->col);
  u->sub = o->kind;
  u->arg = o->arg;
  u->n = (o->col + (uint32_t)neighbour(o->kind)) & MASK;
  u->t = slot;
}

static Code lower(const Prog *p)
{
  Code c = {0};
  uint32_t *rowstart = xrealloc(NULL, (p->nrows + 1) * sizeof *rowstart);
  uint32_t *stamp = xrealloc(NULL, TAPE * sizeof *stamp), *writer = xrealloc(NULL, TAPE * sizeof *writer);
  uint32_t maxops = 0, r, i, j;
  int32_t *out;
  uint32_t *indeg, *slot;
  uint8_t *done;

  for (r = 0; r < p->nrows; r++) if (p->rows[r].nops > maxops) maxops = p->rows[r].nops;
  out = xrealloc(NULL, (maxops + 1) * sizeof *out);
  indeg = xrealloc(NULL, (maxops + 1) * sizeof *indeg);
  slot = xrealloc(NULL, (maxops + 1) * sizeof *slot);
  done = xrealloc(NULL, maxops + 1);
  memset(stamp, 0, TAPE * sizeof *stamp);

  for (r = 0; r < p->nrows; r++) {
    const Row *R = &p->rows[r];
    const Op *ops = p->ops + R->first;
    uint32_t k = R->nops, left = k, ntemp = 0, nio = 0;
    int blocked_input = 0;

    rowstart[r] = c.n;
    for (i = 0; i < k; i++) {
      if (writes(ops[i].kind)) stamp[ops[i].col] = r + 1, writer[ops[i].col] = i;
      if (ops[i].kind >= OP_IN) nio++;
      indeg[i] = 0;
      done[i] = 0;
    }
    for (i = 0; i < k; i++) {
      int nb = neighbour(ops[i].kind);
      uint32_t n = (ops[i].col + (uint32_t)nb) & MASK;
      out[i] = -1;
      if (nb && stamp[n] == r + 1 && writer[n] != i) {
        out[i] = (int32_t)writer[n]; /* i must run before writer[n] overwrites n */
        indeg[writer[n]]++;
      }
    }
    for (i = 0; i < k; i++)
      if (ops[i].kind == OP_IN && indeg[i]) blocked_input = 1;

    if (blocked_input && nio > 1) {
      /* Reordering could swap the order of input and output: buffer every write. */
      for (i = 0; i < k; i++) {
        if (writes(ops[i].kind)) emit_op(&c, &ops[i], 1, slot[i] = ntemp++);
        else emit_op(&c, &ops[i], 0, 0);
      }
      for (i = 0; i < k; i++)
        if (writes(ops[i].kind)) emit(&c, U_COMMIT, ops[i].col)->t = slot[i];
    } else {
      while (left) {
        int temp = 0;
        for (i = 0; i < k; i++) if (!done[i] && !indeg[i]) break;
        if (i == k) { /* every remaining op is on a cycle: defer one write */
          for (i = 0; done[i]; i++) {}
          temp = 1;
        }
        emit_op(&c, &ops[i], temp, temp ? (slot[i] = ntemp++) : 0);
        done[i] = (uint8_t)(1 + temp);
        left--;
        if (out[i] >= 0) indeg[out[i]]--;
      }
      for (i = 0; i < k; i++)
        if (done[i] == 2) emit(&c, U_COMMIT, ops[i].col)->t = slot[i];
    }

    if (R->br && R->slide) {
      emit(&c, U_TEST, R->brcol)->sub = R->br;
      emit(&c, U_SLIDE, 0)->t = (uint32_t)R->slide;
      emit(&c, U_JF, 0)->t = R->target;
    } else if (R->br) {
      emit(&c, R->br == BR_IFZ ? U_JZ : U_JNZ, R->brcol)->t = R->target;
    } else if (R->slide) {
      emit(&c, U_SLIDE, 0)->t = (uint32_t)R->slide;
    }
  }
  rowstart[p->nrows] = c.n;
  emit(&c, U_END, 0);
  for (j = 0; j < c.n; j++)
    if (c.code[j].k == U_JZ || c.code[j].k == U_JNZ || c.code[j].k == U_JF)
      c.code[j].t = rowstart[c.code[j].t];

  free(rowstart); free(stamp); free(writer); free(out); free(indeg); free(slot); free(done);
  return c;
}

/* ------------------------------------------------------------------------ */
/* Interpreter                                                              */

static inline uint8_t temp_eval(uint8_t v, uint8_t w, const UOp *u)
{
  switch (u->sub) {
  case OP_SET:   return v | u->arg;
  case OP_ZERO:  return 0;
  case OP_NOT:   return (uint8_t)~v;
  case OP_SHL:   return (uint8_t)(v << 1);
  case OP_SHR:   return v >> 1;
  case OP_OR:    return v | w;
  case OP_AND:   return v & w;
  case OP_XOR:   return v ^ w;
  case OP_FROMR: case OP_FROML: return w;
  case OP_IN:    return iget();
  }
  return v;
}

/*
 * The dispatch loop lives in a macro so it can be instantiated twice: once for
 * programs that slide (cells are addressed relative to the base) and once for
 * programs that never do (cells are absolute, saving an add and a mask).
 */
#define RUN_LOOP(NAME, CELL)                                                    \
static void NAME(UOp *code, uint8_t *m, uint8_t *tmp)                           \
{                                                                               \
  const UOp *pc = code;                                                         \
  uint32_t b = 0;                                                               \
  int flag = 0;                                                                 \
  static const void *const tbl[] = {                                            \
    &&L_SET, &&L_ZERO, &&L_NOT, &&L_SHL, &&L_SHR, &&L_OR, &&L_AND, &&L_XOR,     \
    &&L_MOV, &&L_IN, &&L_OUT, &&L_NUM, &&L_SKIPIN, &&L_TEMP, &&L_COMMIT,        \
    &&L_JZ, &&L_JNZ, &&L_TEST, &&L_SLIDE, &&L_JF, &&L_END                       \
  };                                                                            \
  UOp *u;                                                                       \
  for (u = code; u->k != U_END; u++) u->h = tbl[u->k];                          \
  u->h = tbl[U_END];                                                            \
  (void)b;                                                                      \
  goto *pc->h;                                                                  \
L_SET:    CELL(pc->a) |= pc->arg; goto *(++pc)->h;                              \
L_ZERO:   CELL(pc->a) = 0; goto *(++pc)->h;                                     \
L_NOT:    CELL(pc->a) = (uint8_t)~CELL(pc->a); goto *(++pc)->h;                 \
L_SHL:    CELL(pc->a) = (uint8_t)(CELL(pc->a) << 1); goto *(++pc)->h;           \
L_SHR:    CELL(pc->a) >>= 1; goto *(++pc)->h;                                   \
L_OR:     CELL(pc->a) |= CELL(pc->n); goto *(++pc)->h;                          \
L_AND:    CELL(pc->a) &= CELL(pc->n); goto *(++pc)->h;                          \
L_XOR:    CELL(pc->a) ^= CELL(pc->n); goto *(++pc)->h;                          \
L_MOV:    CELL(pc->a) = CELL(pc->n); goto *(++pc)->h;                           \
L_IN:     CELL(pc->a) = iget(); goto *(++pc)->h;                                \
L_OUT:    oputc(CELL(pc->a)); goto *(++pc)->h;                                  \
L_NUM:    onum(CELL(pc->a)); goto *(++pc)->h;                                   \
L_SKIPIN: iget(); goto *(++pc)->h;                                              \
L_TEMP:   tmp[pc->t] = temp_eval(CELL(pc->a), CELL(pc->n), pc); goto *(++pc)->h;\
L_COMMIT: CELL(pc->a) = tmp[pc->t]; goto *(++pc)->h;                            \
L_JZ:     pc = CELL(pc->a) ? pc + 1 : code + pc->t; goto *pc->h;                \
L_JNZ:    pc = CELL(pc->a) ? code + pc->t : pc + 1; goto *pc->h;                \
L_TEST:   flag = (pc->sub == BR_IFZ) == (CELL(pc->a) == 0); goto *(++pc)->h;    \
L_SLIDE:  b += pc->t; goto *(++pc)->h;                                          \
L_JF:     pc = flag ? code + pc->t : pc + 1; goto *pc->h;                       \
L_END:    return;                                                               \
}

#if defined(__GNUC__)
#define CELL_ABS(x) m[x]
#define CELL_REL(x) m[(b + (x)) & MASK]
RUN_LOOP(run_abs, CELL_ABS)
RUN_LOOP(run_rel, CELL_REL)
#else
#error "piebald.c needs GCC or Clang (computed goto)"
#endif

static void run(Code *c, int slides)
{
  uint8_t *m = calloc(TAPE, 1), *tmp = calloc(TAPE, 1);
  if (!m || !tmp) die("piebald: out of memory");
  if (slides) run_rel(c->code, m, tmp);
  else run_abs(c->code, m, tmp);
  oflush();
  free(m);
  free(tmp);
}

/* ------------------------------------------------------------------------ */
/* Compiler to C                                                            */

static const char *prelude =
  "#include <stdio.h>\n"
  "#ifdef _WIN32\n#include <io.h>\n#include <fcntl.h>\n#define isatty _isatty\n#define fileno _fileno\n"
  "#else\n#include <unistd.h>\n#endif\n"
  "typedef unsigned char u8;\n"
  "static char ob[1<<16]; static unsigned ol; static int itty, otty;\n"
  "static void fl(void){ if (ol) fwrite(ob,1,ol,stdout); ol=0; fflush(stdout); }\n";
static const char *helper_oc =
  "static void oc(u8 c){ ob[ol++]=(char)c; if (ol==sizeof ob || (otty && c=='\\n')) fl(); }\n";
static const char *helper_on =
  "static void on(u8 v){ if(v>=100) oc('0'+v/100); if(v>=10) oc('0'+v/10%10); oc('0'+v%10); }\n";
static const char *helper_ic =
  "static u8 ic(void){ int c; if (itty) fl(); c=getchar(); return c==EOF?0:(u8)c; }\n";

static void cell_expr(char *buf, const Prog *p, uint32_t col)
{
  if (p->slides) sprintf(buf, "M(%uu)", col);
  else if (col < p->width) sprintf(buf, "m[%u]", col);
  else strcpy(buf, "0"); /* never-written cell beyond the program */
}

static void compile(const Prog *p, FILE *out)
{
  uint8_t *target = xrealloc(NULL, p->nrows + 1);
  uint32_t r, i;
  char a[32], nb[32];

  memset(target, 0, p->nrows + 1);
  for (r = 0; r < p->nrows; r++)
    if (p->rows[r].br) target[p->rows[r].target] = 1;

  int used_oc = 0, used_on = 0, used_ic = 0;
  for (i = 0; i < p->nops; i++) {
    int k = p->ops[i].kind;
    used_on |= k == OP_NUM;
    used_oc |= k == OP_OUT || k == OP_NUM;
    used_ic |= k == OP_IN || k == OP_SKIPIN;
  }
  fprintf(out, "/* Generated by piebald -c from %s */\n%s%s%s%s", p->file, prelude,
          used_oc ? helper_oc : "", used_on ? helper_on : "", used_ic ? helper_ic : "");
  if (p->slides)
    fprintf(out, "static u8 m[%u];\n#define M(c) m[(unsigned short)(b + (c))]\n", TAPE);
  fprintf(out, "int main(void)\n{\n");
  if (p->slides) fprintf(out, "  unsigned short b = 0;\n");
  else fprintf(out, "  u8 m[%u] = {0};\n", p->width ? p->width : 1);
  fprintf(out,
          "#ifdef _WIN32\n  _setmode(0, _O_BINARY); _setmode(1, _O_BINARY);\n#endif\n"
          "  itty = isatty(fileno(stdin)); otty = isatty(fileno(stdout));\n");

  for (r = 0; r < p->nrows; r++) {
    const Row *R = &p->rows[r];
    if (target[r]) fprintf(out, "R%u:", r);
    fprintf(out, " { /* line %d */\n", R->line);
    if (R->br) {
      cell_expr(a, p, R->brcol);
      fprintf(out, "    int j = %s%s;\n", R->br == BR_IFZ ? "!" : "!!", a);
    }
    for (i = R->first; i < R->first + R->nops; i++) {
      const Op *o = &p->ops[i];
      int n = neighbour(o->kind);
      cell_expr(a, p, o->col);
      if (n) cell_expr(nb, p, (o->col + (uint32_t)n) & MASK);
      switch (o->kind) {
      case OP_OUT: fprintf(out, "    oc(%s);\n", a); continue;
      case OP_NUM: fprintf(out, "    on(%s);\n", a); continue;
      case OP_SKIPIN: fprintf(out, "    ic();\n"); continue;
      }
      fprintf(out, "    u8 t%u = ", o->col);
      switch (o->kind) {
      case OP_SET:   fprintf(out, "%s | %u;\n", a, o->arg); break;
      case OP_ZERO:  fprintf(out, "0;\n"); break;
      case OP_NOT:   fprintf(out, "~%s;\n", a); break;
      case OP_SHL:   fprintf(out, "%s << 1;\n", a); break;
      case OP_SHR:   fprintf(out, "%s >> 1;\n", a); break;
      case OP_OR:    fprintf(out, "%s | %s;\n", a, nb); break;
      case OP_AND:   fprintf(out, "%s & %s;\n", a, nb); break;
      case OP_XOR:   fprintf(out, "%s ^ %s;\n", a, nb); break;
      case OP_FROMR: case OP_FROML: fprintf(out, "%s;\n", nb); break;
      case OP_IN:    fprintf(out, "ic();\n"); break;
      }
    }
    for (i = R->first; i < R->first + R->nops; i++) {
      const Op *o = &p->ops[i];
      if (!writes(o->kind)) continue;
      cell_expr(a, p, o->col);
      fprintf(out, "    %s = t%u;\n", a, o->col);
    }
    if (R->slide) fprintf(out, "    b += %d;\n", R->slide);
    if (R->br) fprintf(out, "    if (j) goto R%u;\n", R->target);
    fprintf(out, "  }\n");
  }
  if (target[p->nrows]) fprintf(out, "R%u:\n", p->nrows);
  fprintf(out, "  fl();\n  return 0;\n}\n");
  free(target);
}

/* ------------------------------------------------------------------------ */

static void usage(const char *argv0)
{
  fprintf(stderr,
          "usage: %s [-c] [-O0] [-s] program.pie\n"
          "  -c   write an equivalent C program to stdout instead of running\n"
          "  -O0  disable optimisations\n"
          "  -s   print execution statistics to stderr\n", argv0);
  exit(2);
}

int main(int argc, char *argv[])
{
  Prog p;
  const char *file = NULL;
  int emit = 0, opt = 1, stats = 0, i;

  for (i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-c")) emit = 1;
    else if (!strcmp(argv[i], "-O0")) opt = 0;
    else if (!strcmp(argv[i], "-s")) stats = 1;
    else if (argv[i][0] == '-' && argv[i][1]) usage(argv[0]);
    else if (file) usage(argv[0]);
    else file = argv[i];
  }
  if (!file) usage(argv[0]);

  parse(&p, file);
  {
    uint32_t rows0 = p.nrows, ops0 = p.nops;
    if (opt && !p.slides) eliminate_dead_wires(&p);
    if (opt) compact_rows(&p);
    if (stats)
      fprintf(stderr, "piebald: %u rows, %u instructions; after optimisation %u rows, %u instructions\n",
              rows0, ops0, p.nrows, p.nops);
  }

  if (emit) {
    compile(&p, stdout);
    return 0;
  }

#ifdef _WIN32
  _setmode(0, _O_BINARY);
  _setmode(1, _O_BINARY);
#endif
  stdin_tty = isatty(fileno(stdin));
  stdout_tty = isatty(fileno(stdout));
  {
    Code c = lower(&p);
    if (stats) fprintf(stderr, "piebald: %u micro-ops\n", c.n);
    run(&c, p.slides);
  }
  return 0;
}
