---
description: 'Page detailing the ClickHouse query analyzer'
keywords: ['analyzer']
sidebar_label: 'Analyzer'
slug: /operations/analyzer
title: 'Analyzer'
---

# Analyzer

In ClickHouse version `24.3`, the new query analyzer was enabled by default.
You can read more details about how it works [here](/guides/developer/understanding-query-execution-with-the-analyzer#analyzer).

## Known incompatibilities {#known-incompatibilities}

Despite fixing a large number of bugs and introducing new optimizations, it also introduces some breaking changes in ClickHouse behaviour. Please read the following changes to determine how to rewrite your queries for the new analyzer.

### Invalid queries are no longer optimized {#invalid-queries-are-no-longer-optimized}

The previous query planning infrastructure applied AST-level optimizations before the query validation step.
Optimizations could rewrite the initial query to be valid and executable.

In the new analyzer, query validation takes place before the optimization step.
This means that invalid queries which were previously possible to execute, are now unsupported.
In such cases, the query must be fixed manually.

#### Example 1 {#example-1}

The following query uses column `number` in the projection list when only `toString(number)` is available after the aggregation.
In the old analyzer, `GROUP BY toString(number)` was optimized into `GROUP BY number,` making the query valid.

```sql
SELECT number
FROM numbers(1)
GROUP BY toString(number)
```

#### Example 2 {#example-2}

The same problem occurs in this query. Column `number` is used after aggregation with another key.
The previous query analyzer fixed this query by moving the `number > 5` filter from the `HAVING` clause to the `WHERE` clause.

```sql
SELECT
    number % 2 AS n,
    sum(number)
FROM numbers(10)
GROUP BY n
HAVING number > 5
```

To fix the query, you should move all conditions that apply to non-aggregated columns to the `WHERE` section to conform to standard SQL syntax:

```sql
SELECT
    number % 2 AS n,
    sum(number)
FROM numbers(10)
WHERE number > 5
GROUP BY n
```

### `CREATE VIEW` with an invalid query {#create-view-with-invalid-query}

The new analyzer always performs type-checking.
Previously, it was possible to create a `VIEW` with an invalid `SELECT` query.
It would then fail during the first `SELECT` or `INSERT` (in the case of `MATERIALIZED VIEW`).

It is no longer possible to create a `VIEW` in this way.

#### Example {#example-view}

```sql
CREATE TABLE source (data String)
ENGINE=MergeTree
ORDER BY tuple();

CREATE VIEW some_view
AS SELECT JSONExtract(data, 'test', 'DateTime64(3)')
FROM source;
```

### Known incompatibilities of the `JOIN` clause {#known-incompatibilities-of-the-join-clause}

#### `JOIN` using a column from a projection {#join-using-column-from-projection}

An alias from the `SELECT` list can not be used as a `JOIN USING` key by default.

A new setting, `analyzer_compatibility_join_using_top_level_identifier`, when enabled, alters the behavior of `JOIN USING` to prefer resolving identifiers based on expressions from the projection list of the `SELECT` query, rather than using the columns from the left table directly.

For example:

```sql
SELECT a + 1 AS b, t2.s
FROM VALUES('a UInt64, b UInt64', (1, 1)) AS t1
JOIN VALUES('b UInt64, s String', (1, 'one'), (2, 'two')) t2
USING (b);
```

With `analyzer_compatibility_join_using_top_level_identifier` set to `true`, the join condition is interpreted as `t1.a + 1 = t2.b`, matching the behavior of the earlier versions.
The result will be `2, 'two'`.
When the setting is `false`, the join condition defaults to `t1.b = t2.b`, and the query will return `2, 'one'`.
If `b` is not present in `t1`, the query will fail with an error.

#### Changes in behavior with `JOIN USING` and `ALIAS`/`MATERIALIZED` columns {#changes-in-behavior-with-join-using-and-aliasmaterialized-columns}

In the new analyzer, using `*` in a `JOIN USING` query that involves `ALIAS` or `MATERIALIZED` columns will include those columns in the result-set by default.

For example:

```sql
CREATE TABLE t1 (id UInt64, payload ALIAS sipHash64(id)) ENGINE = MergeTree ORDER BY id;
INSERT INTO t1 VALUES (1), (2);

CREATE TABLE t2 (id UInt64, payload ALIAS sipHash64(id)) ENGINE = MergeTree ORDER BY id;
INSERT INTO t2 VALUES (2), (3);

SELECT * FROM t1
FULL JOIN t2 USING (payload);
```

In the new analyzer, the result of this query will include the `payload` column along with `id` from both tables.
In contrast, the previous analyzer would only include these `ALIAS` columns if specific settings (`asterisk_include_alias_columns` or `asterisk_include_materialized_columns`) were enabled,
and the columns might appear in a different order.

To ensure consistent and expected results, especially when migrating old queries to the new analyzer, it is advisable to specify columns explicitly in the `SELECT` clause rather than using `*`.

#### Handling of type modifiers for columns in the `USING` clause {#handling-of-type-modifiers-for-columns-in-using-clause}

In the new version of the analyzer, the rules for determining the common supertype for columns specified in the `USING` clause have been standardized to produce more predictable outcomes,
especially when dealing with type modifiers like `LowCardinality` and `Nullable`.

- `LowCardinality(T)` and `T`: When a column of type `LowCardinality(T)` is joined with a column of type `T`, the resulting common supertype will be `T`, effectively discarding the `LowCardinality` modifier.
- `Nullable(T)` and `T`: When a column of type `Nullable(T)` is joined with a column of type `T`, the resulting common supertype will be `Nullable(T)`, ensuring that the nullable property is preserved.

For example:

```sql
SELECT id, toTypeName(id)
FROM VALUES('id LowCardinality(String)', ('a')) AS t1
FULL OUTER JOIN VALUES('id String', ('b')) AS t2
USING (id);
```

In this query, the common supertype for `id` is determined as `String`, discarding the `LowCardinality` modifier from `t1`.

### Projection column names changes {#projection-column-names-changes}

During projection names computation, aliases are not substituted.

```sql
SELECT
    1 + 1 AS x,
    x + 1
SETTINGS enable_analyzer = 0
FORMAT PrettyCompact

   ┌─x─┬─plus(plus(1, 1), 1)─┐
1. │ 2 │                   3 │
   └───┴─────────────────────┘

SELECT
    1 + 1 AS x,
    x + 1
SETTINGS enable_analyzer = 1
FORMAT PrettyCompact

   ┌─x─┬─plus(x, 1)─┐
1. │ 2 │          3 │
   └───┴────────────┘
```

### Incompatible function arguments types {#incompatible-function-arguments-types}

In the new analyzer, type inference happens during initial query analysis.
This change means that type checks are done before short-circuit evaluation; thus, the `if` function arguments must always have a common supertype.

For example, the following query fails with `There is no supertype for types Array(UInt8), String because some of them are Array and some of them are not`:

```sql
SELECT toTypeName(if(0, [2, 3, 4], 'String'))
```

### Heterogeneous clusters {#heterogeneous-clusters}

The new analyzer significantly changes the communication protocol between servers in the cluster. Thus, it's impossible to run distributed queries on servers with different `enable_analyzer` setting values.

### Mutations are interpreted by previous analyzer {#mutations-are-interpreted-by-previous-analyzer}

Mutations are still using the old analyzer.
This means some new ClickHouse SQL features can't be used in mutations. For example, the `QUALIFY` clause.
The status can be checked [here](https://github.com/ClickHouse/ClickHouse/issues/61563).

### Unsupported features {#unsupported-features}

The list of features that the new analyzer currently doesn't support is given below:

- Hypothesis index. Work in progress [here](https://github.com/ClickHouse/ClickHouse/pull/48381).
- Window view is not supported. There are no plans to support it in the future.
