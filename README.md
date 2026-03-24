# `sqlite-mmr`

A small SQLite extension for Maximal Marginal Relevance (MMR) reranking,
Jaccard similarity, and FTS5 token extraction.

- Wraps any MATCH-capable table (FTS5, etc.) with a diversity-aware reranker
- Overfetches candidates, tokenizes text, and uses greedy MMR to promote topical variety
- `match_tokens(fts)`: FTS5 auxiliary function that reads matched tokens from the index (no content decompression)
- `tokenize(text)`: scalar function returning sorted deduplicated lowercase tokens
- `jaccard(a, b)`: scalar function computing Jaccard similarity on token strings
- Written in pure C, no dependencies beyond SQLite
- Single file (`sqlite-mmr.c`), compiles to a ~20KB shared library

## The problem

Search engines rank results by relevance, which tends to cluster topically
similar documents at the top. If your top 10 results for "memory" are all
slight variations of the same page, you're missing useful results further down
the list.

## The solution

`sqlite-mmr` provides a virtual table wrapper that sits on top of any
MATCH-capable source table. It overfetches candidates, computes pairwise
Jaccard similarity between token sets, and runs a greedy MMR selection loop
to balance relevance against diversity.

For FTS5, `match_tokens(fts)` reads matched tokens directly from the inverted
index via `xInstToken`, avoiding content decompression entirely. This makes
MMR reranking fast even on large compressed archives.

## Sample usage

```sql
.load ./mmr0

CREATE VIRTUAL TABLE docs USING fts5(body);

INSERT INTO docs(rowid, body) VALUES
  (1, 'how to take care of your cat and keep them healthy'),
  (2, 'best cat food brands for indoor cats and kittens'),
  (3, 'fun cat toys and games for cats to play with'),
  (4, 'how to take care of your dog and keep them healthy'),
  (5, 'popular cat breeds like siamese persian and maine coon'),
  (6, 'common cat health issues and veterinary care tips'),
  (7, 'grooming tips for long hair cats and short hair cats');

CREATE VIRTUAL TABLE docs_mmr USING mmr(
  docs,
  match_tokens(docs),
  rank
);

SELECT rowid, text FROM docs_mmr
  WHERE text MATCH 'cat' AND k = 5 AND mmr_lambda = 1.0;

SELECT rowid, text FROM docs_mmr
  WHERE text MATCH 'cat' AND k = 5 AND mmr_lambda = 0.5;

SELECT tokenize('Hello World hello');         -- 'hello world'
SELECT jaccard('a b c', 'b c d');             -- 0.5
SELECT match_tokens(docs) FROM docs WHERE docs MATCH 'cat';
```

## API

### Virtual table

```sql
CREATE VIRTUAL TABLE <name> USING mmr(
    <source_table>,
    <text_expr>,
    <rank_expr>
);
```

| Parameter | Description |
|-----------|-------------|
| `source_table` | Name of the source table (must support `MATCH`) |
| `text_expr` | SQL expression for similarity input (e.g., `match_tokens(fts)`, `snippet()`, column name) |
| `rank_expr` | SQL expression for relevance scoring |

### Query columns

| Column | Type | Hidden | Description |
|--------|------|--------|-------------|
| `rank` | REAL | yes | Relevance score from source |
| `text` | TEXT | no | Text from source (via `text_expr`) |
| `k` | INT | yes | Number of results to return (required) |
| `mmr_lambda` | REAL | yes | `1.0` = pure relevance, `0.5` = balanced, `0.0` = pure diversity (default `1.0`) |

### Scalar functions

| Function | Description |
|----------|-------------|
| `tokenize(text)` | Returns sorted, deduplicated, lowercase tokens separated by spaces |
| `jaccard(a, b)` | Jaccard similarity between two space-separated token strings |

### FTS5 auxiliary function

| Function | Description |
|----------|-------------|
| `match_tokens(fts)` | Returns space-separated unique matched tokens from the FTS5 index via `xInstToken`. No content decompression. |

### How MMR works

When `mmr_lambda < 1.0`:

1. Overfetch `k * 5` candidates from source table
2. Tokenize each text result (lowercase, split on non-alphanumeric, deduplicate)
3. Normalize ranks to relevance scores in `[0, 1]`
4. Greedy selection loop picks the candidate maximizing:

   `score = lambda * relevance - (1 - lambda) * max_jaccard_to_selected`

5. Return the top `k` selected rows

When `mmr_lambda >= 1.0`: no reranking, returns top `k` by rank directly.

## Building

```sh
make            # builds mmr0.so (or mmr0.dylib on macOS)
make test       # runs smoke tests
make install    # installs to /usr/local/lib and /usr/local/include
```

Requires a C compiler and SQLite development headers (`sqlite3ext.h`).

For static linking:

```sh
make static     # compiles sqlite-mmr.o with -DSQLITE_CORE
```

## License

BSD 3-Clause. See [LICENSE](LICENSE).
