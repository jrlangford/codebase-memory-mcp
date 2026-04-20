# Behaviour Validation

BehaviourDocs are markdown files that declare an authoritative relationship between documentation and code. They let cbm answer: *"what behaviour is specified for this code, and what tests verify it?"*

## Schema

A BehaviourDoc is any markdown file with YAML frontmatter containing `specifies` and/or `prescribes` keys:

```yaml
---
specifies:
  - qn: "internal.business.cases.priority_order.HighestPriorityOrder"
  - qn: "internal.business.cases.priority_order.NewHighestPriorityOrder"
  - qn: "internal.business.cases.priority_order.PriorityOrder"
prescribes:
  - qn: "internal.business.cases.priority_order_test.Test_HighestPriorityOrder"
---

# Highest-Priority Order Selection
...
```

### Fields

- **specifies** — repo-relative qualified names of the code this doc describes. Targets are concrete implementations (classes, functions, methods), not ports or interfaces.
- **prescribes** — repo-relative qualified names of test functions that *must* verify the specified behaviour.

Qualified names use dot-separated path segments without the project prefix: `internal.business.cases.priority_order.HighestPriorityOrder`. The indexer prepends the project name at resolve time.

### Placement

BehaviourDocs live under `docs/behaviour/` in each service repo. One doc per behaviour boundary (a use case, a domain rule, a stateful protocol).

## Edge Types

cbm produces three documentation-related edge types, ordered by commitment strength:

| Edge | Source | Target | Meaning |
|---|---|---|---|
| `REFERENCES` | any Module | any node | Markdown contains an inline `[text](path)` link to the target. Weak — just a hyperlink. |
| `SPECIFIES` | BehaviourDoc | Class, Function, Method | "This doc authoritatively describes this code's behaviour." |
| `PRESCRIBES` | BehaviourDoc | Function (test) | "This test must verify the specified behaviour." |

Additionally, cbm's existing `TESTS` and `TESTS_FILE` edges (from `pass_tests`) capture which tests *actually call* the code, independent of what's prescribed.

## Validation Queries

### Is this function covered by a behaviour spec?

```cypher
MATCH (d:BehaviourDoc)-[:SPECIFIES]->(c)
WHERE c.name = "HighestPriorityOrder"
RETURN d.name
```

### What tests are prescribed for this behaviour?

```cypher
MATCH (d:BehaviourDoc)-[:PRESCRIBES]->(t)
WHERE d.name CONTAINS "priority_order"
RETURN t.name, t.file_path
```

### Prescribed vs actual test coverage

```cypher
MATCH (d:BehaviourDoc)-[:SPECIFIES]->(c)
WHERE c.name = "PriorityOrder"
OPTIONAL MATCH (d)-[:PRESCRIBES]->(prescribed)
OPTIONAL MATCH (actual)-[:TESTS]->(c)
RETURN c.name,
       collect(DISTINCT prescribed.name) AS prescribed_tests,
       collect(DISTINCT actual.name) AS actual_tests
```

Gaps to look for:
- **Prescribed but not actually testing** — the spec says this test covers the code, but the test doesn't call it. Stale or aspirational.
- **Actually testing but not prescribed** — real coverage that nobody documented. Fine, but invisible to reviewers.

### Which BehaviourDocs are affected by a change?

```cypher
MATCH (d:BehaviourDoc)-[:SPECIFIES]->(c)
WHERE c.file_path IN ["internal/business/cases/priority_order.go"]
RETURN d.name, collect(c.name) AS affected_functions
```

Use `detect_changes` to get the changed file list from a branch or PR, then feed it into this query.

### Undocumented high-risk code

```cypher
MATCH (c:Class)
WHERE NOT (c)<-[:SPECIFIES]-(:BehaviourDoc)
RETURN c.name, c.file_path
ORDER BY c.name
```

Cross-reference with Louvain community analysis to prioritize: high-connectivity classes without docs are the biggest risk.

## PR Workflow Integration

When reviewing or creating a PR, agents should check:

1. **Does this PR change code covered by a BehaviourDoc?** Run `detect_changes` on the PR branch, then query for BehaviourDocs that SPECIFIES anything in the changed files. If yes, flag the doc for review — the behaviour contract may need updating.

2. **Does this PR add new behaviour?** If the PR introduces a new use case, domain rule, or stateful protocol, a BehaviourDoc should be created alongside the code.

3. **Are prescribed tests still valid?** If the PR modifies a function that's SPECIFIES'd by a BehaviourDoc, check whether the PRESCRIBES'd tests still call the modified code. Test renames or signature changes can silently break the prescribed relationship.

4. **Short-circuit on empty:** If `search_graph(label="BehaviourDoc")` returns zero results for a project, skip all BehaviourDoc checks — the project hasn't adopted them yet.

## MCP Tool Usage

Agents access these queries through the `codebase-memory-mcp` MCP tools:

- **`search_graph(label="BehaviourDoc", project="...")`** — find all BehaviourDocs in a project
- **`query_graph(query="MATCH ...", project="...")`** — run any Cypher query from above
- **`detect_changes(repo_path="...", branch="...")`** — get changed files for impact analysis
- **`trace_path(function_name="...", mode="calls")`** — trace call chains from a function to find transitive impacts

## Graph UI

BehaviourDoc nodes appear in the graph UI with:
- **Pink** label color (`#ec4899`) in the filter panel and tooltips
- **Size 12.0** in the 3D layout (between Package and File)
- Visible in both **RAW** and **RUNTIME** modes (not excluded as scaffolding)
- **SPECIFIES** edges render in pink, **PRESCRIBES** in rose

Use the **Isolate** toggle to strip the graph down to just BehaviourDoc relationships: disable all edge types except SPECIFIES and PRESCRIBES, enable Isolate, then click Fit.
