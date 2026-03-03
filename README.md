# `sqlite-jmmr`

A small SQLite extension that adds Jaccard-based Maximal Marginal Relevance
(MMR) reranking to search results.

- Wraps any MATCH-capable table (FTS5, etc.) with a diversity-aware reranker
- Overfetches candidates, tokenizes text, and uses greedy MMR to promote topical variety
- Configurable text and rank expressions — works with `snippet()`, plain columns, or any SQL expression
- Written in pure C, no dependencies beyond SQLite — runs anywhere SQLite runs
- Single file (`sqlite-jmmr.c`), compiles to a ~20KB shared library

## The problem

Search engines rank results by relevance, which tends to cluster topically
similar documents at the top. If your top 10 results for "memory" are all
slight variations of the same page, you're missing useful results further down
the list.

## The solution

`sqlite-jmmr` provides a virtual table wrapper that sits on top of any
MATCH-capable source table. It overfetches candidates, tokenizes the text of
each result, computes pairwise Jaccard similarity between token sets, and runs a
greedy MMR selection loop to balance relevance against diversity.

The interface uses a hidden column pattern for `mmr_lambda` — balancing
relevance against diversity via a single tuning parameter.

## Sample usage

```sql
.load ./jmmr0

-- Create an FTS5 table
CREATE VIRTUAL TABLE docs_fts USING fts5(title, body);

INSERT INTO docs_fts(rowid, title, body) VALUES
  (1, 'cat care',     'how to take care of your cat and keep them healthy'),
  (2, 'cat food',     'best cat food brands for indoor cats and kittens'),
  (3, 'cat toys',     'fun cat toys and games for cats to play with'),
  (4, 'dog care',     'how to take care of your dog and keep them healthy'),
  (5, 'cat breeds',   'popular cat breeds like siamese persian and maine coon'),
  (6, 'cat health',   'common cat health issues and veterinary care tips'),
  (7, 'cat grooming', 'grooming tips for long hair cats and short hair cats');

-- Create the wrapper table (text_expr = snippet on body column)
CREATE VIRTUAL TABLE docs_fts_mmr USING jaccard_mmr(
  docs_fts,
  snippet(docs_fts, 1, '>>>', '<<<', '...', 16),
  rank
);

-- Pure relevance (lambda=1.0): same order as FTS5
SELECT rowid, text FROM docs_fts_mmr
  WHERE text MATCH 'cat' AND k = 5 AND mmr_lambda = 1.0;

-- Balanced diversity (lambda=0.5): topically diverse results
SELECT rowid, text FROM docs_fts_mmr
  WHERE text MATCH 'cat' AND k = 5 AND mmr_lambda = 0.5;
```

## API

### Creating a wrapper table

```sql
CREATE VIRTUAL TABLE <name> USING jaccard_mmr(
    <source_table>,
    <text_expr>,
    <rank_expr>
);
```

| Parameter | Description |
|-----------|-------------|
| `source_table` | Name of the source table (must support `MATCH`) |
| `text_expr` | SQL expression for the text to tokenize and return as `text` |
| `rank_expr` | SQL expression for relevance scoring |

The `text_expr` can be any valid SQL expression that produces text — a column
name, `snippet()`, string concatenation, etc. It is evaluated in the context of
a query on `source_table`.

### Querying

```sql
SELECT rowid, rank, text FROM <name>
  WHERE text MATCH :query
    AND k = :k
    AND mmr_lambda = :lambda;
```

| Column | Type | Hidden | Description |
|--------|------|--------|-------------|
| `rank` | REAL | yes | Relevance score from source (via `rank_expr`) |
| `text` | TEXT | no | Text from source (via `text_expr`) |
| `k` | INT | yes | Number of results to return (required) |
| `mmr_lambda` | REAL | yes | Diversity: `1.0` = pure relevance, `0.5` = balanced, `0.0` = pure diversity (optional, default `1.0`) |

**Constraints:** `text MATCH` and `k` are required. `mmr_lambda` is optional.

### How MMR works

When `mmr_lambda < 1.0`:

1. Overfetch `k * 5` candidates from source table
2. Tokenize each text result (lowercase, split on non-alphanumeric, deduplicate)
3. Normalize ranks to relevance scores in `[0, 1]`
4. Greedy selection loop — at each step, pick the unselected candidate that maximizes:

   `score = lambda * relevance - (1 - lambda) * max_jaccard_to_selected`

5. Return the top `k` selected rows

When `mmr_lambda >= 1.0`: no reranking, returns top `k` by rank directly.

## Building

```sh
make            # builds jmmr0.so (or jmmr0.dylib on macOS)
make test       # runs smoke tests
make install    # installs to /usr/local/lib and /usr/local/include
```

Requires a C compiler and SQLite development headers (`sqlite3ext.h`).

For static linking into an application:

```sh
make static     # compiles sqlite-jmmr.o with -DSQLITE_CORE
```

## License

BSD 3-Clause. See [LICENSE](LICENSE).
