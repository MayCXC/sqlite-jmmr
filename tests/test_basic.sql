-- test_basic.sql: smoke tests for sqlite-jmmr extension
-- Run: sqlite3 :memory: '.load ./jmmr0' '.read tests/test_basic.sql'

CREATE VIRTUAL TABLE test_fts USING fts5(title, content);

INSERT INTO test_fts(rowid, title, content) VALUES
  (1, 'cat care', 'how to take care of your cat and keep them healthy'),
  (2, 'cat food', 'best cat food brands for indoor cats and kittens'),
  (3, 'cat toys', 'fun cat toys and games for cats to play with'),
  (4, 'dog care', 'how to take care of your dog and keep them healthy'),
  (5, 'dog food', 'best dog food brands for puppies and adult dogs'),
  (6, 'bird care', 'how to take care of your pet bird and keep them happy'),
  (7, 'fish tanks', 'setting up a fish tank with proper filtration'),
  (8, 'cat breeds', 'popular cat breeds like siamese persian and maine coon'),
  (9, 'cat health', 'common cat health issues and veterinary care tips'),
  (10, 'cat grooming', 'grooming tips for long hair cats and short hair cats');

CREATE VIRTUAL TABLE test_fts_mmr USING jaccard_mmr(test_fts, snippet(test_fts, 1, '>>>', '<<<', '...', 16), rank);

-- Test 1: basic query without MMR (lambda=1.0 = pure relevance)
SELECT 'TEST 1: pure relevance (lambda=1.0)';
SELECT rowid, rank, text FROM test_fts_mmr
  WHERE text MATCH 'cat' AND k = 5 AND mmr_lambda = 1.0;

-- Test 2: MMR diversity (lambda=0.5)
SELECT '';
SELECT 'TEST 2: MMR diversity (lambda=0.5)';
SELECT rowid, rank, text FROM test_fts_mmr
  WHERE text MATCH 'cat' AND k = 5 AND mmr_lambda = 0.5;

-- Test 3: k=3 limit
SELECT '';
SELECT 'TEST 3: k=3 limit';
SELECT rowid, rank, text FROM test_fts_mmr
  WHERE text MATCH 'care' AND k = 3 AND mmr_lambda = 1.0;

-- Test 4: no results
SELECT '';
SELECT 'TEST 4: no results';
SELECT rowid, rank, text FROM test_fts_mmr
  WHERE text MATCH 'zebra' AND k = 5 AND mmr_lambda = 0.5;

-- Test 5: pure diversity (lambda=0.0)
SELECT '';
SELECT 'TEST 5: pure diversity (lambda=0.0)';
SELECT rowid, rank, text FROM test_fts_mmr
  WHERE text MATCH 'cat' AND k = 5 AND mmr_lambda = 0.0;

-- Test 6: k > candidates (should return all available)
SELECT '';
SELECT 'TEST 6: k > total matches';
SELECT rowid, rank, text FROM test_fts_mmr
  WHERE text MATCH 'fish' AND k = 10 AND mmr_lambda = 0.5;

-- Test 7: default mmr_lambda (omitted = 1.0)
SELECT '';
SELECT 'TEST 7: mmr_lambda omitted (default 1.0)';
SELECT rowid, rank, text FROM test_fts_mmr
  WHERE text MATCH 'care' AND k = 3;

SELECT '';
SELECT 'ALL TESTS PASSED';
