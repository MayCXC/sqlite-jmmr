# `sqlite-jmmr`

Jaccard-based Maximal Marginal Relevance (MMR) reranking for SQLite. Wraps any
MATCH-capable table (FTS5, etc.) with a diversity-aware reranker using a hidden
`mmr_lambda` column.

Relevance-ranked search tends to cluster similar documents at the top.
`sqlite-jmmr` overfetches candidates, tokenizes each result, computes pairwise
Jaccard similarity, and greedily selects a diverse subset. The `mmr_lambda`
parameter controls the tradeoff: `1.0` for pure relevance, `0.0` for pure
diversity, `0.5` for balanced.

- Works with `snippet()`, plain columns, or any SQL expression as text source
- Pure C, no dependencies beyond SQLite, runs anywhere SQLite runs
- Single file, compiles to a ~20KB shared library

## Usage

```sql
.load ./jmmr0

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

-- Balanced (lambda=0.5): diverse results
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

`text_expr` can be any SQL expression that produces text: a column name,
`snippet()`, string concatenation, etc. Evaluated in the context of a query on
`source_table`.

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
| `mmr_lambda` | REAL | yes | `1.0` = pure relevance, `0.5` = balanced, `0.0` = pure diversity (optional, default `1.0`) |

`text MATCH` and `k` are required. `mmr_lambda` is optional.

### How it works

When `mmr_lambda < 1.0`:

1. Overfetch `k * 5` candidates from source table
2. Tokenize each text result (lowercase, split on non-alphanumeric, deduplicate)
3. Normalize ranks to relevance scores in `[0, 1]`
4. Greedy selection: pick the unselected candidate maximizing:

   `score = lambda * relevance - (1 - lambda) * max_jaccard_to_selected`

5. Return the top `k` selected rows

When `mmr_lambda >= 1.0`: returns top `k` by rank directly, no reranking.

## Building

```sh
make            # builds jmmr0.so (or jmmr0.dylib on macOS)
make test       # runs smoke tests
make install    # installs to /usr/local/lib and /usr/local/include
```

Requires a C compiler and SQLite development headers (`sqlite3ext.h`).

For static linking:

```sh
make static     # compiles sqlite-jmmr.o with -DSQLITE_CORE
```

## License

BSD 3-Clause. See [LICENSE](LICENSE).
