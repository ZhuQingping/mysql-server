# Extended Secondary Partition Types Feature Specification

> Version: 1.0
> Target: MySQL 8.0 (Huawei RDS Branch)
> Purpose: Enable an AI coding tool to accurately re-implement this feature on a clean MySQL 8.0 codebase

---

## 1. Overview

### 1.1 Problem Statement

MySQL 8.0 only supports a limited set of subpartition type combinations. When a table uses HASH or KEY as the primary partition type, the subpartition type is restricted to HASH or KEY as well. The combinations of RANGE-RANGE, RANGE-LIST, LIST-RANGE, LIST-LIST, KEY-RANGE, KEY-LIST, HASH-RANGE, HASH-LIST, and KEY-HASH are not supported. This limits users who need fine-grained data distribution across two partitioning dimensions.

### 1.2 Solution

Extend the partition type system to support all combinations of primary and secondary partition types. When `rds_extended_partitions_enabled` is set to ON, the server allows RANGE and LIST subpartition types under any primary partition type, including HASH and KEY. This requires:

1. Modifying the `subpartition_type` field in the `mysql.tables` DD table from the original ENUM to one that includes RANGE, LIST, RANGE_COLUMNS, and LIST_COLUMNS subpartition types.
2. Incrementing `TAURUS_EXTENDED_DD_VERSION` from 0 to 1 (and then 2 for subsequent fixes) to trigger automatic DD schema upgrade.
3. Adding validation logic in the parser and partition info handling to allow or reject combinations based on the flag.
4. Handling partition definition (part_def) parsing for subpartitions with RANGE/LIST types.

### 1.3 Supported Combinations

When `rds_extended_partitions_enabled = OFF` (default, MySQL 8.0 behavior):

| Primary \ Sub | HASH | KEY | RANGE | LIST |
|---------------|------|-----|-------|------|
| RANGE | Yes | Yes | No | No |
| LIST | Yes | Yes | No | No |
| HASH | No | No | No | No |
| KEY | No | No | No | No |

When `rds_extended_partitions_enabled = ON`:

| Primary \ Sub | HASH | KEY | RANGE | LIST |
|---------------|------|-----|-------|------|
| RANGE | Yes | Yes | Yes | Yes |
| LIST | Yes | Yes | Yes | Yes |
| HASH | Yes | Yes | Yes | Yes |
| KEY | Yes | Yes | Yes | Yes |

All 16 combinations are supported.

### 1.4 Code Volume

| Category | Files | Lines of Code |
|----------|-------|---------------|
| SQL layer (parser, partition_info, DD) | ~20 | ~1,500 |
| DD schema changes (table_impl, tables.cc) | ~10 | ~300 |
| Test result files | ~80 | ~58,000 |
| Test case files | ~10 | ~3,500 |
| **Total** | **~122** | **~63,658 / -1,509** |

Most of the line count comes from test result files that reflect the new DD schema.

---

## 2. New Files

No entirely new files are created. All changes are modifications to existing files.

---

## 3. Modifications to Existing MySQL Files

### 3.1 `sql/dd/dd_version.h`

Increment `TAURUS_EXTENDED_DD_VERSION`:

```c++
// Before:
static const uint TAURUS_EXTENDED_DD_VERSION = 0;

// After:
#ifdef NDEBUG
static const uint TAURUS_EXTENDED_DD_VERSION = 2;
#else
static uint TAURUS_EXTENDED_DD_VERSION = 2;
#endif
```

Version history:
- **Version 1**: Initial support for extended subpartition types. Modified `subpartition_type` field's ENUM type in `mysql.tables` to include RANGE, LIST, RANGE_COLUMNS, LIST_COLUMNS.
- **Version 2**: Bug fix for crash when subpartition type is LIST/RANGE with no partition definition.

`TAURUS_EXTENDED_DD_VERSION` works alongside `DD_VERSION` to indicate the version of the data dictionary. Incrementing it triggers an automatic upgrade of the `mysql.tables` table schema in existing databases.

### 3.2 `sql/dd/types/table.h`

The `enum_partition_type` already includes RANGE and LIST types. No change needed to the enum itself:

```c++
enum enum_partition_type {
  PT_NONE = 0,
  PT_HASH,
  PT_KEY_51,
  PT_KEY_55,
  PT_LINEAR_HASH,
  PT_LINEAR_KEY_51,
  PT_LINEAR_KEY_55,
  PT_RANGE,
  PT_LIST,
  PT_RANGE_COLUMNS,
  PT_LIST_COLUMNS,
  PT_AUTO,
  PT_AUTO_LINEAR,
};
```

The virtual methods already exist:

```c++
virtual enum_partition_type partition_type() const = 0;
virtual void set_partition_type(enum_partition_type partition_type) = 0;
virtual enum_partition_type subpartition_type() const = 0;
virtual void set_subpartition_type(enum_partition_type subpartition_type) = 0;
```

### 3.3 `sql/dd/impl/types/table_impl.h` / `table_impl.cc`

The `subpartition_type` field's storage in the DD table is changed. Previously, the ENUM in `mysql.tables` only supported HASH/KEY subpartition types. Now it includes all partition types:

```c++
// In table_impl.h:
enum_partition_type m_subpartition_type;  // Already exists, now supports RANGE/LIST

// In table_impl.cc - serialization:
r->store(Tables::FIELD_SUBPARTITION_TYPE, m_subpartition_type,
         m_subpartition_type == PT_NONE);
```

### 3.4 `sql/dd/impl/tables/tables.cc`

The DD table definition for `subpartition_type` is updated to include all partition type values:

```c++
m_target_def.add_field(FIELD_SUBPARTITION_TYPE, "FIELD_SUBPARTITION_TYPE",
                       "subpartition_type ENUM(\n"
                       "  'HASH','KEY_51','KEY_55',\n"
                       "  'LINEAR_HASH','LINEAR_KEY_51',\n"
                       "  'LINEAR_KEY_55','RANGE','LIST',\n"
                       "  'RANGE_COLUMNS','LIST_COLUMNS',\n"
                       "  'AUTO', 'AUTO_LINEAR'"
                       ")");
```

The key change is that RANGE, LIST, RANGE_COLUMNS, and LIST_COLUMNS are now valid values for the `subpartition_type` column. The `TAURUS_EXTENDED_DD_VERSION` bump causes MySQL to automatically alter this column when upgrading.

### 3.5 `sql/partition_info.cc`

New validation function:

```c++
bool validate_partition_type(partition_info *part_info) {
  if (part_info && part_info->is_sub_partitioned() &&
      !extended_partitions_enabled &&
      (part_info->part_type == partition_type::HASH ||
       part_info->sub_part_info->part_type != partition_type::HASH)) {
    return false;
  }
  return true;
}
```

Logic:
- When `extended_partitions_enabled = OFF`:
  - HASH-primary partitioned tables cannot have subpartitions at all
  - Non-HASH subpartition types (RANGE, LIST) are not allowed under any primary type
- When `extended_partitions_enabled = ON`:
  - All combinations are allowed

### 3.6 `sql/parse_tree_partitions.cc`

In the `PT_partition::contextualize()` method, the validation for subpartition definitions is updated:

```c++
if (part_info.sub_part_info != nullptr) {
  if (part_info.sub_part_info->part_type == partition_type::RANGE ||
      part_info.sub_part_info->part_type == partition_type::LIST) {
    if (!extended_partitions_enabled) {
      my_error(ER_PARTITION_SUBPARTITION_ERROR, MYF(0));
    } else {
      // RANGE/LIST subpartitions require explicit partition definitions
      if (part_info.sub_part_info->part_type == partition_type::RANGE) {
        my_error(ER_PARTITIONS_MUST_BE_DEFINED_ERROR, MYF(0), "RANGE");
      } else if (part_info.sub_part_info->part_type ==
                 partition_type::LIST) {
        my_error(ER_PARTITIONS_MUST_BE_DEFINED_ERROR, MYF(0), "LIST");
      }
    }
    return true;
  }
}
```

When a RANGE or LIST subpartition type is used without explicit partition definitions (part_defs is null), an error is raised. RANGE and LIST subpartitions always require explicit partition definitions, unlike HASH/KEY which can have auto-generated partitions.

### 3.7 `sql/mysqld.h` / `sql/mysqld.cc`

New global variable:

```c++
// In mysqld.h:
extern bool extended_partitions_enabled;

// In mysqld.cc:
bool extended_partitions_enabled = false;
```

### 3.8 `sql/sys_vars.cc`

New system variable:

```c++
static Sys_var_bool Sys_extended_partitions_enabled(
    "rds_extended_partitions_enabled",
    "Enable extended secondary partition type combinations.",
    GLOBAL_VAR(extended_partitions_enabled), CMD_LINE(OPT_ARG), DEFAULT(false));
```

### 3.9 `sql/dd/impl/types/partition_impl.h` / `partition_impl.cc`

Partition and subpartition DD objects must properly handle RANGE and LIST type subpartitions, including their boundary expressions and value lists.

### 3.10 `sql/dd/dd_table.cc`

The `get_partition_type()` function and partition metadata handling must correctly map between the parser's partition type representation and the DD's `enum_partition_type` for subpartitions.

### 3.11 `sql/sql_partition.cc`

Partition DDL handling (CREATE TABLE, ALTER TABLE) must support creating and modifying subpartitions with RANGE and LIST types, including:
- Parsing VALUES LESS THAN / VALUES IN clauses for subpartition definitions
- Building the correct partition element tree
- Generating proper DD metadata

### 3.12 `sql/dd/impl/bootstrap/bootstrapper.cc` / `sql/dd/impl/upgrade/dd.cc`

The DD upgrade logic must handle the `TAURUS_EXTENDED_DD_VERSION` increment by altering the `mysql.tables` table schema to add the new ENUM values for `subpartition_type`.

### 3.13 `sql/dd/impl/tables/dd_properties.cc`

Register the extended DD version for upgrade tracking.

---

## 4. Core Data Structures

### 4.1 enum_partition_type

The existing partition type enum already includes all needed values. The key change is that `subpartition_type` in the DD can now use values beyond HASH/KEY:

```c++
enum enum_partition_type {
  PT_NONE = 0,
  PT_HASH,             // Used for both primary and subpartition
  PT_KEY_51,           // KEY version 5.1
  PT_KEY_55,           // KEY version 5.5
  PT_LINEAR_HASH,      // LINEAR HASH
  PT_LINEAR_KEY_51,    // LINEAR KEY 5.1
  PT_LINEAR_KEY_55,    // LINEAR KEY 5.5
  PT_RANGE,            // NEW for subpartition use
  PT_LIST,             // NEW for subpartition use
  PT_RANGE_COLUMNS,    // NEW for subpartition use
  PT_LIST_COLUMNS,     // NEW for subpartition use
  PT_AUTO,             // Auto-determined
  PT_AUTO_LINEAR       // Auto-determined linear
};
```

### 4.2 partition_info

The existing `partition_info` class already supports `sub_part_info` (a nested `partition_info` for the subpartition level). No structural changes are needed; the change is in validation rules.

### 4.3 TAURUS_EXTENDED_DD_VERSION

```c++
#ifdef NDEBUG
static const uint TAURUS_EXTENDED_DD_VERSION = 2;
#else
static uint TAURUS_EXTENDED_DD_VERSION = 2;
#endif
```

This version number is separate from `DD_VERSION` and controls Huawei-specific DD schema changes. Incrementing it triggers an automatic ALTER TABLE on `mysql.tables` to update the `subpartition_type` ENUM column.

---

## 5. Execution Flow

### 5.1 CREATE TABLE with Extended Subpartitions

```
CREATE TABLE t1 (...)
  PARTITION BY RANGE (col1)
  SUBPARTITION BY RANGE (col2)
  SUBPARTITIONS 3
  (PARTITION p0 VALUES LESS THAN (10),
   PARTITION p1 VALUES LESS THAN (20));

Parser (sql/parse_tree_partitions.cc):
  -> PT_partition::contextualize()
     -> Create partition_info with part_type = RANGE
     -> Create sub_part_info with part_type = RANGE
     -> Validate:
        - extended_partitions_enabled must be ON
        - RANGE/LIST subpartitions require explicit definitions
        - Part_defs must specify VALUES clauses for both levels

Optimizer (sql/partition_info.cc):
  -> validate_partition_type(part_info)
     -> If extended_partitions_enabled = OFF: reject
     -> If ON: allow all combinations

DD Storage (sql/dd/dd_table.cc):
  -> Set table->set_subpartition_type(PT_RANGE)
  -> Store partition elements with subpartition boundary values
```

### 5.2 SELECT with Extended Subpartitions

```
SELECT * FROM t1 WHERE col1 < 10 AND col2 < 5;

Partition pruning:
  -> Primary level: RANGE on col1 -> prune to p0
  -> Subpartition level: RANGE on col2 -> prune to specific subpartition(s)
  -> Both levels participate in pruning
```

### 5.3 DD Version Upgrade

```
On startup:
  -> Check stored DD version vs current DD_VERSION + TAURUS_EXTENDED_DD_VERSION
  -> If TAURUS_EXTENDED_DD_VERSION increased:
     -> ALTER TABLE mysql.tables
        MODIFY COLUMN subpartition_type
        ENUM('HASH','KEY_51','KEY_55',
             'LINEAR_HASH','LINEAR_KEY_51',
             'LINEAR_KEY_55','RANGE','LIST',
             'RANGE_COLUMNS','LIST_COLUMNS',
             'AUTO','AUTO_LINEAR')
  -> Update stored version number
```

---

## 6. System Variables

| Variable | Type | Scope | Default | Dynamic | Description |
|----------|------|-------|---------|---------|-------------|
| `rds_extended_partitions_enabled` | bool | GLOBAL | false | Yes | Enable extended subpartition type combinations |

Startup option: `--rds-extended-partitions-enabled`

---

## 7. EXPLAIN Output

No new EXPLAIN output fields. The existing partition pruning information in EXPLAIN naturally extends to handle two-level RANGE/LIST pruning. Both the primary and subpartition levels are shown in the `partition` column.

---

## 8. Error Handling

| Error Condition | Handling |
|----------------|----------|
| RANGE/LIST subpartition with `extended_partitions_enabled=OFF` | `ER_PARTITION_SUBPARTITION_ERROR` |
| RANGE/LIST subpartition without explicit partition definitions | `ER_PARTITIONS_MUST_BE_DEFINED_ERROR` |
| Crash when subpartition type is LIST/RANGE with no `part_def` defined | Fixed by validating `part_def` before dereference (version 2 fix) |
| Invalid partition combination | Parser-level error |

---

## 9. Memory Management

No new memory allocation patterns. The partition_info structure is allocated and managed by the existing parser/optimizer infrastructure. Subpartition info for RANGE/LIST types follows the same lifecycle as HASH/KEY subpartitions.

---

## 10. Test Coverage

The commit includes 122 files with extensive test result updates. Key test areas:

| Category | Description |
|----------|-------------|
| Range-Range subpartition | CREATE TABLE with RANGE primary + RANGE sub |
| Range-List subpartition | CREATE TABLE with RANGE primary + LIST sub |
| List-Range subpartition | CREATE TABLE with LIST primary + RANGE sub |
| List-List subpartition | CREATE TABLE with LIST primary + LIST sub |
| Hash-Range subpartition | CREATE TABLE with HASH primary + RANGE sub |
| Hash-List subpartition | CREATE TABLE with HASH primary + LIST sub |
| Key-Range subpartition | CREATE TABLE with KEY primary + RANGE sub |
| Key-List subpartition | CREATE TABLE with KEY primary + LIST sub |
| Key-Hash subpartition | CREATE TABLE with KEY primary + HASH sub |
| Hash-Hash subpartition | CREATE TABLE with HASH primary + HASH sub |
| Disabled mode | Verify errors when `rds_extended_partitions_enabled=OFF` |
| DD upgrade | Verify schema upgrade on version change |
| Partition pruning | Verify pruning works for two-level RANGE/LIST |
| DML operations | INSERT/SELECT/UPDATE/DELETE on extended partition tables |
| ALTER TABLE | Add/drop/modify partitions with RANGE/LIST subpartitions |
| Crash fix (version 2) | Verify no crash when LIST/RANGE subpartition with no part_def |
| Information schema | Verify PARTITIONS table shows correct subpartition info |

---

## 11. Limitations

- `rds_extended_partitions_enabled` must be ON to use extended combinations
- RANGE and LIST subpartitions always require explicit partition definitions (unlike HASH/KEY which can auto-generate)
- The `subpartition_type` DD column ENUM change requires a server restart to take effect (DD upgrade)
- `TAURUS_EXTENDED_DD_VERSION` does not support downgrade
- Subpartition with RANGE/LIST type under a HASH/KEY primary partition requires the user to explicitly define all subpartition boundaries within each primary partition
- Column-level (RANGE COLUMNS, LIST COLUMNS) subpartitions follow the same rules as their non-COLUMNS counterparts
- Changing `rds_extended_partitions_enabled` at runtime only affects new DDL statements; existing tables are not affected

---

## 12. Implementation Order

Recommended order for implementing this feature on a clean codebase:

1. **`sql/dd/dd_version.h`** — Increment `TAURUS_EXTENDED_DD_VERSION` to 1
2. **`sql/dd/impl/tables/tables.cc`** — Update `subpartition_type` ENUM definition to include RANGE, LIST, RANGE_COLUMNS, LIST_COLUMNS
3. **`sql/dd/impl/types/table_impl.h/.cc`** — Ensure serialization/deserialization handles new subpartition types
4. **`sql/dd/impl/types/partition_impl.h/.cc`** — Handle RANGE/LIST subpartition boundary values in DD
5. **`sql/dd/dd_table.cc`** — Update `get_partition_type()` and subpartition handling
6. **`sql/mysqld.h` / `sql/mysqld.cc`** — Add `extended_partitions_enabled` global variable
7. **`sql/sys_vars.cc`** — Define `rds_extended_partitions_enabled` system variable
8. **`sql/partition_info.cc`** — Add `validate_partition_type()` function
9. **`sql/parse_tree_partitions.cc`** — Update parser validation for RANGE/LIST subpartitions
10. **`sql/sql_partition.cc`** — Handle RANGE/LIST subpartition DDL operations
11. **`sql/dd/impl/bootstrap/bootstrapper.cc`** — DD upgrade bootstrap
12. **`sql/dd/impl/upgrade/dd.cc`** — DD upgrade logic
13. **`sql/dd/impl/tables/dd_properties.cc`** — Register extended DD version
14. **Bug fix (version 2)** — Fix crash when subpartition type is LIST/RANGE with no part_def defined
15. **MTR test suite** — Port test cases for all combination types
