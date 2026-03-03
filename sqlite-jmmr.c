/*
** sqlite-jmmr.c — Jaccard MMR virtual table wrapper for FTS5.
**
** Provides Jaccard-similarity-based Maximal Marginal Relevance (MMR)
** reranking for FTS5 search results.  Mirrors the sqlite-vec mmr_lambda
** pattern: overfetch from FTS5, rerank internally, return diversified
** results.
**
** CREATE VIRTUAL TABLE t_mmr USING jaccard_mmr(t_fts, snippet_col_idx);
**
** SELECT rowid, rank, snippet FROM t_mmr
**   WHERE snippet MATCH :q AND k = :k AND mmr_lambda = :lambda;
**
** BSD 3-Clause License. See LICENSE for details.
*/

#include "sqlite-jmmr.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#ifndef SQLITE_CORE
#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT1
#else
#include "sqlite3.h"
#endif

/* ---- Error helper (same pattern as sqlite-vec) ----------------------- */

static void vtab_set_error(sqlite3_vtab *pVTab, const char *zFormat, ...) {
  va_list args;
  sqlite3_free(pVTab->zErrMsg);
  va_start(args, zFormat);
  pVTab->zErrMsg = sqlite3_vmprintf(zFormat, args);
  va_end(args);
}

/* ---- Token set -------------------------------------------------------- */

typedef struct jmmr_tokenset {
  char **tokens;
  int n;
  int cap;
} jmmr_tokenset;

static void jmmr_tokenset_init(jmmr_tokenset *ts) {
  ts->tokens = NULL;
  ts->n = 0;
  ts->cap = 0;
}

static void jmmr_tokenset_free(jmmr_tokenset *ts) {
  for (int i = 0; i < ts->n; i++)
    sqlite3_free(ts->tokens[i]);
  sqlite3_free(ts->tokens);
  ts->tokens = NULL;
  ts->n = ts->cap = 0;
}

static int jmmr_tokenset_push(jmmr_tokenset *ts, const char *tok, int len) {
  if (ts->n >= ts->cap) {
    int newcap = ts->cap ? ts->cap * 2 : 16;
    char **p = sqlite3_realloc(ts->tokens, newcap * sizeof(char *));
    if (!p)
      return SQLITE_NOMEM;
    ts->tokens = p;
    ts->cap = newcap;
  }
  char *s = sqlite3_malloc(len + 1);
  if (!s)
    return SQLITE_NOMEM;
  memcpy(s, tok, len);
  s[len] = '\0';
  ts->tokens[ts->n++] = s;
  return SQLITE_OK;
}

static int cmp_str(const void *a, const void *b) {
  return strcmp(*(const char **)a, *(const char **)b);
}

/*
** Tokenize text: lowercase, split on non-alnum/underscore, dedup via sort.
*/
static int jmmr_tokenset_tokenize(jmmr_tokenset *ts, const char *text) {
  if (!text)
    return SQLITE_OK;
  const char *p = text;
  while (*p) {
    while (*p && !(isalnum((unsigned char)*p) || *p == '_'))
      p++;
    if (!*p)
      break;
    const char *start = p;
    while (*p && (isalnum((unsigned char)*p) || *p == '_'))
      p++;
    int len = (int)(p - start);
    if (len == 0)
      continue;
    char buf[256];
    if (len > 255)
      len = 255;
    for (int i = 0; i < len; i++)
      buf[i] = (char)tolower((unsigned char)start[i]);
    int rc = jmmr_tokenset_push(ts, buf, len);
    if (rc != SQLITE_OK)
      return rc;
  }
  /* sort */
  if (ts->n > 1)
    qsort(ts->tokens, ts->n, sizeof(char *), cmp_str);
  /* dedup (adjacent unique) */
  if (ts->n > 0) {
    int w = 1;
    for (int r = 1; r < ts->n; r++) {
      if (strcmp(ts->tokens[r], ts->tokens[w - 1]) != 0) {
        if (w != r)
          ts->tokens[w] = ts->tokens[r];
        w++;
      } else {
        sqlite3_free(ts->tokens[r]);
      }
    }
    ts->n = w;
  }
  return SQLITE_OK;
}

/*
** Jaccard similarity between two sorted, deduplicated token sets.
** Computed via sorted merge — O(n+m).
*/
static double jmmr_jaccard(jmmr_tokenset *a, jmmr_tokenset *b) {
  if (a->n == 0 && b->n == 0)
    return 0.0;
  int i = 0, j = 0, inter = 0;
  while (i < a->n && j < b->n) {
    int c = strcmp(a->tokens[i], b->tokens[j]);
    if (c == 0) {
      inter++;
      i++;
      j++;
    } else if (c < 0) {
      i++;
    } else {
      j++;
    }
  }
  int uni = a->n + b->n - inter;
  return uni > 0 ? (double)inter / (double)uni : 0.0;
}

/* ---- Row buffer ------------------------------------------------------- */

typedef struct jmmr_row {
  sqlite3_int64 rowid;
  double bm25_rank;
  char *snippet;
  jmmr_tokenset tokens;
  int selected;
} jmmr_row;

static void jmmr_row_free(jmmr_row *r) {
  sqlite3_free(r->snippet);
  jmmr_tokenset_free(&r->tokens);
}

/* ---- Virtual table ---------------------------------------------------- */

typedef struct jmmr_vtab {
  sqlite3_vtab base;
  sqlite3 *db;
  char *fts_table;
  int snippet_col;
} jmmr_vtab;

typedef struct jmmr_cursor {
  sqlite3_vtab_cursor base;
  jmmr_row *rows;
  int n_rows;
  int current;
} jmmr_cursor;

/* ---- xCreate / xConnect ---------------------------------------------- */

static int jmmrInit(sqlite3 *db, void *pAux, int argc,
                    const char *const *argv, sqlite3_vtab **ppVtab,
                    char **pzErr) {
  (void)pAux;
  if (argc < 4) {
    *pzErr = sqlite3_mprintf("jaccard_mmr: expected (fts_table, snippet_col)");
    return SQLITE_ERROR;
  }

  const char *fts_table = argv[3];
  int snippet_col = 0;
  if (argc >= 5)
    snippet_col = atoi(argv[4]);

  /*
  ** Schema:
  **   rank       REAL  HIDDEN  (col 0) — BM25 rank from FTS5
  **   snippet    TEXT          (col 1) — snippet(fts, col, ...)
  **   k          INT   HIDDEN  (col 2) — result count
  **   mmr_lambda REAL  HIDDEN  (col 3) — 1.0=relevance, 0.5=balanced
  */
  int rc = sqlite3_declare_vtab(db,
    "CREATE TABLE x("
    "rank REAL HIDDEN, "
    "snippet TEXT, "
    "k INT HIDDEN, "
    "mmr_lambda REAL HIDDEN)");
  if (rc != SQLITE_OK)
    return rc;

  jmmr_vtab *pNew = sqlite3_malloc(sizeof(*pNew));
  if (!pNew)
    return SQLITE_NOMEM;
  memset(pNew, 0, sizeof(*pNew));
  pNew->db = db;
  pNew->fts_table = sqlite3_mprintf("%s", fts_table);
  pNew->snippet_col = snippet_col;

  if (!pNew->fts_table) {
    sqlite3_free(pNew);
    return SQLITE_NOMEM;
  }

  *ppVtab = &pNew->base;
  return SQLITE_OK;
}

static int jmmrDestroy(sqlite3_vtab *pVtab) {
  jmmr_vtab *p = (jmmr_vtab *)pVtab;
  sqlite3_free(p->fts_table);
  sqlite3_free(p);
  return SQLITE_OK;
}

/* ---- xBestIndex ------------------------------------------------------ */

/*
** idxStr encoding: 4 bytes per constraint (matching sqlite-vec pattern).
**   byte 0: kind character
**   bytes 1-3: filler ('_')
**
** Kind characters:
**   'M' = MATCH query
**   'K' = k (result count)
**   'L' = mmr_lambda (diversity parameter)
**
** MATCH + k are required.  mmr_lambda is optional (default 1.0).
*/

#define JMMR_IDXSTR_KIND_MATCH  'M'
#define JMMR_IDXSTR_KIND_K      'K'
#define JMMR_IDXSTR_KIND_LAMBDA 'L'

static int jmmrBestIndex(sqlite3_vtab *pVtab, sqlite3_index_info *pInfo) {
  (void)pVtab;
  int iMatch = -1, iK = -1, iLambda = -1;

  for (int i = 0; i < pInfo->nConstraint; i++) {
    if (!pInfo->aConstraint[i].usable)
      continue;
    int col = pInfo->aConstraint[i].iColumn;
    int op = pInfo->aConstraint[i].op;

    if (op == SQLITE_INDEX_CONSTRAINT_MATCH && (col == 1 || col == -1)) {
      iMatch = i;
    } else if (col == 2 && op == SQLITE_INDEX_CONSTRAINT_EQ) {
      iK = i;
    } else if (col == 3 && op == SQLITE_INDEX_CONSTRAINT_EQ) {
      iLambda = i;
    }
  }

  if (iMatch < 0 || iK < 0) {
    pInfo->estimatedCost = 1e12;
    return SQLITE_OK;
  }

  /* Build idxStr: 4 bytes per constraint, '_' padding */
  sqlite3_str *idxStr = sqlite3_str_new(NULL);
  int argvIndex = 1;

  sqlite3_str_appendchar(idxStr, 1, JMMR_IDXSTR_KIND_MATCH);
  sqlite3_str_appendchar(idxStr, 3, '_');
  pInfo->aConstraintUsage[iMatch].argvIndex = argvIndex++;
  pInfo->aConstraintUsage[iMatch].omit = 1;

  sqlite3_str_appendchar(idxStr, 1, JMMR_IDXSTR_KIND_K);
  sqlite3_str_appendchar(idxStr, 3, '_');
  pInfo->aConstraintUsage[iK].argvIndex = argvIndex++;
  pInfo->aConstraintUsage[iK].omit = 1;

  if (iLambda >= 0) {
    sqlite3_str_appendchar(idxStr, 1, JMMR_IDXSTR_KIND_LAMBDA);
    sqlite3_str_appendchar(idxStr, 3, '_');
    pInfo->aConstraintUsage[iLambda].argvIndex = argvIndex++;
    pInfo->aConstraintUsage[iLambda].omit = 1;
  }

  pInfo->idxStr = sqlite3_str_finish(idxStr);
  pInfo->needToFreeIdxStr = 1;
  pInfo->estimatedCost = 100.0;
  pInfo->estimatedRows = 10;
  return SQLITE_OK;
}

/* ---- xOpen / xClose -------------------------------------------------- */

static int jmmrOpen(sqlite3_vtab *pVtab, sqlite3_vtab_cursor **ppCur) {
  (void)pVtab;
  jmmr_cursor *pCur = sqlite3_malloc(sizeof(*pCur));
  if (!pCur)
    return SQLITE_NOMEM;
  memset(pCur, 0, sizeof(*pCur));
  *ppCur = &pCur->base;
  return SQLITE_OK;
}

static int jmmrClose(sqlite3_vtab_cursor *pCur) {
  jmmr_cursor *p = (jmmr_cursor *)pCur;
  for (int i = 0; i < p->n_rows; i++)
    jmmr_row_free(&p->rows[i]);
  sqlite3_free(p->rows);
  sqlite3_free(p);
  return SQLITE_OK;
}

/* ---- xFilter (main query logic) -------------------------------------- */

static int jmmrFilter(sqlite3_vtab_cursor *pCur, int idxNum,
                      const char *idxStr, int argc, sqlite3_value **argv) {
  (void)idxNum;
  jmmr_cursor *cur = (jmmr_cursor *)pCur;
  jmmr_vtab *vtab = (jmmr_vtab *)pCur->pVtab;

  /* Free any previous results */
  for (int i = 0; i < cur->n_rows; i++)
    jmmr_row_free(&cur->rows[i]);
  sqlite3_free(cur->rows);
  cur->rows = NULL;
  cur->n_rows = 0;
  cur->current = 0;

  /* Parse constraints from idxStr + argv (4 bytes per constraint) */
  const char *match_text = NULL;
  int k = 10;
  double mmr_lambda = 1.0;

  for (int i = 0; i < argc; i++) {
    char kind = idxStr[i * 4];
    switch (kind) {
    case JMMR_IDXSTR_KIND_MATCH:
      match_text = (const char *)sqlite3_value_text(argv[i]);
      break;
    case JMMR_IDXSTR_KIND_K:
      k = sqlite3_value_int(argv[i]);
      if (k < 1)
        k = 1;
      break;
    case JMMR_IDXSTR_KIND_LAMBDA:
      mmr_lambda = sqlite3_value_double(argv[i]);
      break;
    }
  }

  if (!match_text || !match_text[0])
    return SQLITE_OK;

  /* Overfetch: k*5 candidates for MMR reranking */
  int fetch_limit = (mmr_lambda < 1.0) ? k * 5 : k;
  if (fetch_limit < k)
    fetch_limit = k;

  /* Prepare internal FTS5 query */
  char *sql = sqlite3_mprintf(
      "SELECT rowid, rank, snippet(\"%w\", %d, '>>>', '<<<', '...', 16) "
      "FROM \"%w\" WHERE \"%w\" MATCH ?1 ORDER BY rank LIMIT %d",
      vtab->fts_table, vtab->snippet_col, vtab->fts_table, vtab->fts_table,
      fetch_limit);
  if (!sql)
    return SQLITE_NOMEM;

  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(vtab->db, sql, -1, &stmt, NULL);
  sqlite3_free(sql);
  if (rc != SQLITE_OK) {
    vtab_set_error(&vtab->base, "%s", sqlite3_errmsg(vtab->db));
    return rc;
  }

  sqlite3_bind_text(stmt, 1, match_text, -1, SQLITE_TRANSIENT);

  /* Fetch all candidates */
  int cap = fetch_limit > 0 ? fetch_limit : 64;
  jmmr_row *rows = sqlite3_malloc(cap * sizeof(jmmr_row));
  if (!rows) {
    sqlite3_finalize(stmt);
    return SQLITE_NOMEM;
  }
  int n = 0;

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    if (n >= cap) {
      cap *= 2;
      jmmr_row *p = sqlite3_realloc(rows, cap * sizeof(jmmr_row));
      if (!p) {
        for (int i = 0; i < n; i++)
          jmmr_row_free(&rows[i]);
        sqlite3_free(rows);
        sqlite3_finalize(stmt);
        return SQLITE_NOMEM;
      }
      rows = p;
    }
    rows[n].rowid = sqlite3_column_int64(stmt, 0);
    rows[n].bm25_rank = sqlite3_column_double(stmt, 1);
    const char *snip = (const char *)sqlite3_column_text(stmt, 2);
    rows[n].snippet = sqlite3_mprintf("%s", snip ? snip : "");
    jmmr_tokenset_init(&rows[n].tokens);
    rows[n].selected = 0;
    if (!rows[n].snippet) {
      for (int i = 0; i < n; i++)
        jmmr_row_free(&rows[i]);
      sqlite3_free(rows);
      sqlite3_finalize(stmt);
      return SQLITE_NOMEM;
    }
    n++;
  }
  sqlite3_finalize(stmt);

  if (n == 0) {
    sqlite3_free(rows);
    return SQLITE_OK;
  }

  /* --- MMR reranking --- */
  if (mmr_lambda < 1.0 && n > 1) {
    /* Tokenize all snippets */
    for (int i = 0; i < n; i++) {
      rc = jmmr_tokenset_tokenize(&rows[i].tokens, rows[i].snippet);
      if (rc != SQLITE_OK) {
        for (int j = 0; j < n; j++)
          jmmr_row_free(&rows[j]);
        sqlite3_free(rows);
        return rc;
      }
    }

    /*
    ** Normalize BM25 ranks to relevance [0, 1].
    ** BM25 rank is negative (lower = better).  Find the minimum (most
    ** negative) and normalize: relevance = 1.0 - rank/min_rank.
    */
    double min_rank = rows[0].bm25_rank;
    for (int i = 1; i < n; i++)
      if (rows[i].bm25_rank < min_rank)
        min_rank = rows[i].bm25_rank;

    double *relevance = sqlite3_malloc(n * sizeof(double));
    if (!relevance) {
      for (int i = 0; i < n; i++)
        jmmr_row_free(&rows[i]);
      sqlite3_free(rows);
      return SQLITE_NOMEM;
    }
    for (int i = 0; i < n; i++) {
      relevance[i] =
          (min_rank < 0.0) ? 1.0 - (rows[i].bm25_rank / min_rank) : 1.0;
    }

    /* Greedy MMR selection */
    int actual_k = k < n ? k : n;
    int *order = sqlite3_malloc(actual_k * sizeof(int));
    if (!order) {
      sqlite3_free(relevance);
      for (int i = 0; i < n; i++)
        jmmr_row_free(&rows[i]);
      sqlite3_free(rows);
      return SQLITE_NOMEM;
    }

    int selected_count = 0;
    for (int step = 0; step < actual_k; step++) {
      int best_idx = -1;
      double best_score = -1e18;

      for (int i = 0; i < n; i++) {
        if (rows[i].selected)
          continue;

        /* max Jaccard similarity to any already-selected row */
        double max_sim = 0.0;
        for (int s = 0; s < selected_count; s++) {
          double sim =
              jmmr_jaccard(&rows[i].tokens, &rows[order[s]].tokens);
          if (sim > max_sim)
            max_sim = sim;
        }

        double mmr_score =
            mmr_lambda * relevance[i] - (1.0 - mmr_lambda) * max_sim;

        if (mmr_score > best_score) {
          best_score = mmr_score;
          best_idx = i;
        }
      }

      if (best_idx < 0)
        break;
      rows[best_idx].selected = 1;
      order[selected_count++] = best_idx;
    }

    /* Compact: reorder rows[] to selected order */
    jmmr_row *reordered = sqlite3_malloc(selected_count * sizeof(jmmr_row));
    if (!reordered) {
      sqlite3_free(order);
      sqlite3_free(relevance);
      for (int i = 0; i < n; i++)
        jmmr_row_free(&rows[i]);
      sqlite3_free(rows);
      return SQLITE_NOMEM;
    }
    for (int i = 0; i < selected_count; i++)
      reordered[i] = rows[order[i]];

    /* Free unselected rows */
    for (int i = 0; i < n; i++) {
      if (!rows[i].selected)
        jmmr_row_free(&rows[i]);
    }
    sqlite3_free(rows);
    sqlite3_free(order);
    sqlite3_free(relevance);
    rows = reordered;
    n = selected_count;
  } else {
    /* No MMR: just take top k by rank (already sorted by FTS5) */
    if (n > k) {
      for (int i = k; i < n; i++)
        jmmr_row_free(&rows[i]);
      n = k;
    }
  }

  cur->rows = rows;
  cur->n_rows = n;
  cur->current = 0;
  return SQLITE_OK;
}

/* ---- Cursor navigation ----------------------------------------------- */

static int jmmrEof(sqlite3_vtab_cursor *pCur) {
  return ((jmmr_cursor *)pCur)->current >= ((jmmr_cursor *)pCur)->n_rows;
}

static int jmmrNext(sqlite3_vtab_cursor *pCur) {
  ((jmmr_cursor *)pCur)->current++;
  return SQLITE_OK;
}

static int jmmrRowid(sqlite3_vtab_cursor *pCur, sqlite3_int64 *pRowid) {
  *pRowid = ((jmmr_cursor *)pCur)->rows[((jmmr_cursor *)pCur)->current].rowid;
  return SQLITE_OK;
}

static int jmmrColumn(sqlite3_vtab_cursor *pCur, sqlite3_context *ctx,
                      int col) {
  jmmr_row *row =
      &((jmmr_cursor *)pCur)->rows[((jmmr_cursor *)pCur)->current];
  switch (col) {
  case 0: /* rank */
    sqlite3_result_double(ctx, row->bm25_rank);
    break;
  case 1: /* snippet */
    sqlite3_result_text(ctx, row->snippet, -1, SQLITE_TRANSIENT);
    break;
  case 2: /* k (input-only) */
  case 3: /* mmr_lambda (input-only) */
    sqlite3_result_null(ctx);
    break;
  }
  return SQLITE_OK;
}

/* ---- Module definition ----------------------------------------------- */

static sqlite3_module jmmrModule = {
    /* iVersion    */ 0,
    /* xCreate     */ jmmrInit,
    /* xConnect    */ jmmrInit,
    /* xBestIndex  */ jmmrBestIndex,
    /* xDisconnect */ jmmrDestroy,
    /* xDestroy    */ jmmrDestroy,
    /* xOpen       */ jmmrOpen,
    /* xClose      */ jmmrClose,
    /* xFilter     */ jmmrFilter,
    /* xNext       */ jmmrNext,
    /* xEof        */ jmmrEof,
    /* xColumn     */ jmmrColumn,
    /* xRowid      */ jmmrRowid,
    /* xUpdate     */ 0,
    /* xBegin      */ 0,
    /* xSync       */ 0,
    /* xCommit     */ 0,
    /* xRollback   */ 0,
    /* xFindMethod */ 0,
    /* xRename     */ 0,
    /* xSavepoint  */ 0,
    /* xRelease    */ 0,
    /* xRollbackTo */ 0,
    /* xShadowName */ 0,
#if SQLITE_VERSION_NUMBER >= 3044000
    /* xIntegrity  */ 0,
#endif
};

/* ---- Entry point ----------------------------------------------------- */

SQLITE_JMMR_API int sqlite3_jmmr_init(sqlite3 *db, char **pzErrMsg,
                                       const sqlite3_api_routines *pApi) {
  (void)pzErrMsg;
#ifndef SQLITE_CORE
  SQLITE_EXTENSION_INIT2(pApi);
#endif
  return sqlite3_create_module_v2(db, "jaccard_mmr", &jmmrModule, NULL, NULL);
}
