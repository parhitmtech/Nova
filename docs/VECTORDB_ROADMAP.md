# Vector retrieval roadmap

Approximate nearest neighbour (ANN) search for Nova's ML workflows, built into the runtime as built-ins
next to the tensor ops, with a vector DB layer whose results are TOON (Token-Oriented Object Notation) so
they go into an LLM prompt cheaply. NovaBot's own prompts move to TOON in the same work. Items are in
order: each one builds on the ones before it.

## Scope

- **Vectors are Tensors.** A collection is a `rows x dim` Tensor: a CSV loaded with `tensor_load_csv`, the
  activations of a novatorch layer, or any tensor a program computes. Nova does not compute text embeddings
  in this plan (no embedding model, no `hf.embed`).
- **CPU only.** Search runs on the interpreter's host, multithreaded for batches of queries. GPU kernels are
  later (see "Later").
- **Three indexes:** Flat (exact, the baseline and ground truth), **HNSW** (graph) and **IVF-PQ**
  (inverted lists with product-quantized codes). A ScaNN-style index was considered and dropped: it adds a
  third quantizer and a re-ranking path for a gain that HNSW and IVF-PQ with re-ranking mostly cover.
- **Metrics:** `"l2"` (squared Euclidean), `"ip"` (inner product) and `"cos"` (cosine: vectors normalised
  once at build time, then inner product).
- **No new dependencies.** Plain C++17 and `std::thread`; the build stays one `make`.

## 0. Groundwork

- **Branch** `feat/vector-retrieval` off `main`.
- **Op codes.** New `ANN_*` and `VDB_*` entries in `TensorOp` (`compiler.h`), placed after `TEN_DIV` and
  before `CON_VEC_NEW`, because the interpreter routes every op at or after `CON_VEC_NEW` to the container
  and object handlers. Names go in `TENSOR_OP_MAP`, argument kinds in `tensor_op_signature()` (`T` tensor,
  `n` number, `s` string), so wrong argument types are compile errors like the other built-ins.
- **Index handles are ints**, like `progress_bar`: the ops that return one go in `TENSOR_INT_OPS`, so the
  tensor-free pass never treats an index id as a tensor handle. An index is freed with `ann_free(ix)`.
- **String results.** The vector DB returns TOON text, so the parser gets a fourth result kind next to
  Tensor, int and double: a `TENSOR_STRING_OPS` set whose result slot holds a `strMem` index and whose
  expression type is `string`, the same way `hf.*` calls return text.
- **Files.** `include/ann.h`; `src/runtime/ann_common.cc` (distances, top-k heap, k-means, the thread
  pool), `ann_flat.cc`, `ann_hnsw.cc`, `ann_ivfpq.cc`, `vectordb.cc`, `toon.cc`; `interp_ann.cc` for the
  dispatch, called from `exec_tensor_call` like `exec_container_call`. All are added to the Makefile.
- **Storage.** Building an index copies the Tensor (stored as `double`) once into a contiguous `float`
  array, the layout the distance loops are written for. Vector ids are row numbers: row `i` of the build
  tensor is id `i`, and `ann_add` continues the count.
- **Errors** use the runtime's existing form, `Runtime error at line N: ...`: an unknown metric, a query
  whose width is not the index's dimension, a freed or unknown index, `k` larger than the collection.

## 1. Flat (exact) search

- `ann_flat(Tensor data, string metric) -> int`
- `ann_search(int ix, Tensor queries, int k) -> Tensor`: a `queries x k` Tensor of ids, best first.
- `ann_distances(int ix) -> Tensor`: the scores of the last search on `ix`, same shape (smaller is closer
  for `l2`, larger for `ip` and `cos`).
- `ann_recall(Tensor found, Tensor truth) -> double`: recall@k of one id tensor against another, for tuning
  the approximate indexes against Flat's exact answer.
- `ann_size(int ix) -> int`, `ann_dim(int ix) -> int`, `ann_free(int ix)`.
- Every later index is tested against Flat's results.

## 2. HNSW

- `ann_hnsw(Tensor data, string metric, int M, int ef_construction) -> int`
- Hierarchical navigable small-world graph: `M` links per node per layer (`2M` on layer 0), node levels
  drawn with multiplier `1 / ln M`, neighbours chosen with the standard heuristic (keep a candidate only if
  it is closer to the new node than to every neighbour already kept), which keeps the graph navigable on
  clustered data.
- `ann_set(ix, "ef_search", n)` trades speed for recall at query time (default `max(k, 64)`).
- `ann_add(ix, Tensor rows)` inserts more vectors without a rebuild.
- A visited list with a generation counter per thread, so a search allocates nothing.
- Random levels come from the runtime's fixed seed, so a build is reproducible and golden tests are stable.
- Memory: the float vectors plus about `2M x 4` bytes of links per vector on layer 0.

## 3. IVF-PQ

- `ann_ivfpq(Tensor data, string metric, int nlist, int m) -> int`
- **Coarse quantizer:** k-means (k-means++ seeding, a fixed number of Lloyd iterations, trained on a
  sample when the collection is large) into `nlist` inverted lists.
- **Product quantizer:** each vector's residual from its list centroid is split into `m` subvectors
  (`dim` must divide by `m`; a compile-time constant check where the arguments are literals, a runtime
  error otherwise), and each subvector becomes one byte (256 centroids per subspace). A vector is stored in
  `m` bytes.
- **Search:** probe the `nprobe` closest lists (`ann_set(ix, "nprobe", n)`, default 8); for each list build
  a `m x 256` lookup table of partial distances and score each code with `m` table reads (asymmetric
  distance). For `ip` and `cos` the score splits as `q·c + q·r`, so the same tables work.
- **Re-ranking:** `ann_set(ix, "rerank", r)` keeps the float vectors and re-scores the best `r` candidates
  exactly (the adaptive re-ranking idea from AQR-HNSW, applied here to PQ). Off by default, so the
  memory-saving mode stays the default.
- `ann_add(ix, Tensor rows)` encodes new rows with the trained quantizers (no retraining).
- Memory: `n x m` bytes of codes, plus `nlist x dim` and `m x 256 x dim/m` floats of codebooks (plus
  `n x dim x 4` bytes with re-ranking on).

## 4. Running indexes well

- **Batched search across threads.** `ann_search` with many query rows splits them across
  `std::thread::hardware_concurrency()` threads (capped, overridable with `ann_set(ix, "threads", n)`).
  Each query's result row is written by one thread, so the output is the same for any thread count.
- **`ann_set(ix, key, value)`** is the one tuning call: `ef_search`, `nprobe`, `rerank`, `threads`. An
  unknown key or one the index does not have is a runtime error.
- **Planner:** `ann_auto(Tensor data, string metric, string goal) -> int` picks the index and its
  parameters. `goal` is `"recall"`, `"latency"`, `"memory"` or `"balanced"`.

  | Situation | Choice |
  |---|---|
  | 5,000 vectors or fewer | Flat (exact is already fast) |
  | `recall` or `latency` | HNSW, `M` 16 (32 when `dim` > 256), `ef_construction` 200 |
  | `memory` | IVF-PQ, `nlist` about `4 x sqrt(n)`, `m` the largest divisor of `dim` up to `dim / 4` |
  | `balanced` | HNSW below 1M vectors, IVF-PQ with re-ranking above |

  `ann_info(ix) -> string` reports the choice and parameters as TOON, so a program (or NovaBot) can show
  why an index was picked.
- **Save and load:** `ann_save(ix, string path)` and `ann_load(string path) -> int` in a small binary
  format (magic number, format version, index kind, metric, then the arrays), so an index is built once and
  reused between runs. Paths go through the same workspace rules as `tensor_load_csv`.

## 5. Vector DB and TOON

- **Metadata in TOON.** `vdb_open(int ix, string path) -> int` attaches a metadata table to an index. The
  file is a TOON tabular array, row `i` describing vector `i`:

  ```
  docs[3]{id,label,source}:
    0,cat,train
    1,dog,train
    2,"cat, tabby",test
  ```

  The row count must equal the index size (a runtime error otherwise). `vdb_rows(db) -> int`,
  `vdb_close(db)`.
- **Queries return TOON.** `vdb_query(int db, Tensor q, int k, string fields) -> string`: the hits as one
  TOON table, with only the columns named in `fields` (**field projection**: `"id,label"` sends two columns,
  not the whole row). `rank` and `score` are always available as columns.

  ```
  hits[2]{rank,score,id,label}:
    1,0.9731,0,cat
    2,0.9502,2,"cat, tabby"
  ```

- **Prompt-sized context.** `vdb_context(int db, Tensor q, int k, int max_tokens, double lambda, string fields)
  -> string`:
  1. Fetch `k` candidates from the index.
  2. **MMR de-duplication:** re-order them by maximal marginal relevance, `lambda x sim(q, d) - (1 - lambda)
     x max sim(d, chosen)`, so near-duplicate rows are dropped before they cost tokens (`lambda` 1.0 turns it
     off). Similarities come from the stored vectors (or the PQ reconstruction when IVF-PQ keeps no floats).
  3. **Token-budget packing:** add rows in that order while the TOON table stays within `max_tokens`.
- **`token_count(string) -> int`**: a fast estimate, `ceil(max(chars / 4, words x 1.3))`. The same formula
  is used by the server (section 7), so the runtime and NovaBot agree on budgets.
- **TOON support in C++** covers what this needs: encoding objects, primitive arrays and tabular arrays
  with the full quoting rules (comma, tab and pipe delimiters), and decoding tabular arrays for metadata
  files. Nested list arrays are not decoded; a metadata file that uses them is a clear error.

## 6. The `vectordb` module

- `stdlib/vectordb.nova`, used with `orbit vectordb`, wraps the built-ins in classes:
  - `VectorIndex`: `VectorIndex("hnsw", data, "cos")` and friends, `search`, `distances`, `set`, `add`,
    `save`, `info`, `size`.
  - `VectorDB`: an index plus its metadata, `query` and `context`.
  - `knn_classify(VectorIndex ix, Tensor labels, Tensor q, int k) -> Tensor`: majority vote over the
    neighbours' labels.
- **Examples** (`examples/`):
  - `knn_mnist.nova`: a kNN classifier on the MNIST CSVs, Flat against HNSW against IVF-PQ: accuracy,
    recall and time.
  - `embedding_search.nova`: train a small novatorch MLP, use its hidden layer as the vectors, search
    them.
  - `retrieval_context.nova`: TOON metadata, `vdb_query` with field projection, `vdb_context` with a
    token budget.

## 7. NovaBot: TOON in prompts

- `server/src/lib/toon.js`: a dependency-free encoder (objects, primitive, tabular and list arrays, all
  three delimiters, the quoting and escape rules), `estimateTokens` (the formula in section 5) and
  `encodeCompact(value, { maxTokens })`, which cuts at a line boundary and ends with `… truncated (N more
  lines)`.
- **Tool results** (`routes/agent.js`) are sent as budgeted TOON. This also fixes a bug: today the result is
  `JSON.stringify(result).slice(0, 5000)`, which can cut JSON in the middle of a value.
- **History and memory** read from DynamoDB (`services/agent-store.js`) are encoded as TOON where they enter
  the prompt. The stored items do not change, so existing data keeps working.
- The system prompt gets one sentence explaining the format. The Bedrock request envelope stays JSON: it is
  the API, not prompt text.

## 8. Tests, benchmarks and docs

- **Golden tests** (`compiler/tests/`), deterministic through the fixed seed; recall is printed as a
  threshold check (`true`) rather than a raw number, so small numeric differences across platforms do not
  break them: `ann_flat`, `ann_hnsw`, `ann_ivfpq`, `ann_rerank`, `ann_auto`, `ann_add`, `ann_save_load`,
  `ann_threads_deterministic`, `vdb_query_toon`, `vdb_projection`, `vdb_mmr`, `vdb_context_budget`,
  `toon_quoting`.
- **Error tests:** `error_ann_metric`, `error_ann_dim_mismatch`, `error_ann_freed`, `error_ann_k_too_large`,
  `error_ivfpq_subspaces`, `error_ann_set_key`, `error_vdb_rows_mismatch`, `error_ann_wrong_arg_type`.
- **Server tests:** `server/test/toon.test.js` (every encoder rule, truncation, and the token saving against
  JSON on a history-shaped array).
- **Benchmark:** `compiler/benchmarks/ann/` builds every index on synthetic clustered data and on MNIST and
  prints build time, queries per second, recall@10 and bytes per vector, as a TOON table.
- **Docs:** `docs/VECTORDB.md` (the user guide: built-ins, the module, choosing an index) and the built-in
  list in `SYNTAX.md`. CI needs no change: `make test` runs the new tests.

## How this fits the compiler roadmap

- This is runtime code behind built-ins. The parser only gains table entries and the string result kind,
  so it does not wait for phases 1–7 of [ROADMAP.md](ROADMAP.md), and those phases carry it over the way
  they carry the tensor built-ins: the check pass (phase 2) takes over the signature checks, and Nova IR
  (phase 7) gives the ops a typed form in place of `TENSOR_CALL` slots. The Nova-level API does not change.
- Strings are not freed until phase 4, so every `vdb_query` / `vdb_context` result stays in memory for the
  rest of the run. Fine for scripts and notebooks; a long-running loop of queries should keep its result in
  one variable and wait for phase 4 before running for hours.
- The distance and lookup-table loops are the first candidates for the MLIR / IREE path in section 3 of
  ROADMAP.md when GPU work starts.

## What stays our own

The index implementations (HNSW, IVF-PQ, k-means, the planner), the TOON encoder and decoder, and the
vector DB layer are written for Nova, with no FAISS, hnswlib or ScaNN dependency, so they build anywhere the
compiler builds and stay readable as reference code.

## Costs to plan for

- **Size:** about 2,500–3,000 lines of C++ and 300 of JavaScript, plus tests. Compile time grows by a few
  seconds.
- **Memory:** building copies the tensor once (doubles to floats), so peak memory is about 1.5x the data
  while the build runs. HNSW keeps every float vector; IVF-PQ is the option when that does not fit.
- **Speed limits:** only the call into a built-in goes through the interpreter; the loops are native. A
  Nova loop issuing one query per iteration pays the interpreter per query, so batch queries into one
  tensor where possible.
- **Determinism and threads:** batched search is identical across thread counts by construction; k-means and
  HNSW builds are single-threaded so builds stay reproducible (parallel builds are a later option).

## Later

- GPU (CUDA) distance and lookup-table kernels, then through MLIR / IREE.
- Filtered search (a metadata predicate applied during the search, not after).
- Deletes and updates in HNSW (tombstones and repair).
- Storing indexes in the user's S3 workspace from the IDE, and an IDE panel for index stats.
- A ScaNN-style anisotropic quantizer, if benchmarks show IVF-PQ with re-ranking falls short.
- Text embeddings (`hf.embed`), if Nova programs need to index text directly.
