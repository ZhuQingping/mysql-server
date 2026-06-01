# Native Package Framework Feature Specification

> Version: 2.0
> Target: MySQL 8.0 (Huawei RDS Branch)
> Main Commit: 42618fa556a550a58e8bcf5d2e373bc5742885f9
> Author: zhuqingping
> Merge Date: 2025-03-12
> Purpose: Enable an AI coding tool to accurately re-implement this feature on a clean MySQL 8.0 codebase

---

## 1. Overview

### 1.1 Problem Statement

MySQL supports stored procedures written in SQL, but there is no mechanism for built-in native procedures implemented in C++. Stored procedures have limitations:

1. They cannot perform administrative operations that require direct access to server internals
2. They are interpreted, not compiled, limiting performance for heavy operations
3. They cannot implement custom result set formats or privileged operations
4. Each RDS feature that needs a CALL-able interface must hack the SQL parser independently
5. There is no centralized way to list, manage, or secure built-in procedures

### 1.2 Solution

Implement a Native Package Framework that provides a unified infrastructure for registering and executing built-in C++ procedures. The framework consists of:

1. **Package container** (`Package`): A singleton that stores all registered native procedures, keyed by (schema_name, proc_name)
2. **Procedure interface** (`Proc`): Abstract class defining procedure metadata (parameters, columns, result type) and the parse tree integration
3. **Command interface** (`Sql_cmd_proc`): Abstract class defining procedure execution lifecycle (prepare, execute, send_result, check_access, check_parameter)
4. **Parser integration** (`PT_package_proc`): A uniform parse tree root that bridges the SQL parser to native procedure execution
5. **Common infrastructure** (`component.h/.cc`): Base classes for memory management, string handling, and container types

The framework is designed so that adding a new native procedure requires only:
- Subclass `Proc` (define name, parameters, columns)
- Subclass `Sql_cmd_proc` (implement `pc_execute()`)
- Register the procedure in `package_context_init()`

### 1.3 Execution Model

```
SQL: CALL schema.proc_name(args)

  Parser (sql_yacc.yy)
    |
    v
  Recognizes CALL statement
    |
    v
  sp_name lookup: exist_native_proc(schema, name)?
    |
    YES --> find_native_proc_and_evoke()
    |        |
    |        v
    |      Proc::PT_evoke() --> PT_package_proc(args, proc)
    |        |
    |        v
    |      PT_package_proc::make_cmd()
    |        |
    |        v
    |      Proc::evoke_cmd() --> Sql_cmd_proc subclass
    |        |
    |        v
    |      Sql_cmd_proc::execute()
    |        1. prepare()  --> check_access() + check_parameter()
    |        2. pc_execute()  --> subclass implementation
    |        3. send_result()  --> OK packet or result set
    |
    NO --> Regular stored procedure execution path
```

### 1.4 Code Volume

| Category | Files | Lines of Code |
|----------|-------|---------------|
| Package core (`sql/package/`) | 11 | ~1,500 |
| Common infrastructure (`sql/common/`) | 2 | ~158 |
| Demo procedures (`sql/package/proc_dummy.h/.cc`) | 2 | ~293 |
| Show native procedure (`sql/package/show_native_procedure.h/.cc`) | 2 | ~299 |
| MySQL core modifications | ~6 | ~50 |
| MTR tests | 4 test files | ~200 |
| **Total** | **33 files** | **+2,075 / -10** |

---

## 2. New Files

### 2.1 Common Infrastructure (`sql/common/`)

| File | Lines | Purpose |
|------|-------|---------|
| `component.h` | 104 | Base classes: `PSI_memory_base`, `Disable_copy_base`, `Pair_key_type`, `Pair_key_comparator`, `Pair_key_unordered_map` |
| `component.cc` | 54 | `Pair_key_comparator` case-insensitive string comparison, `std::hash` specialization for `Pair_key_type` |

### 2.2 Package Core (`sql/package/`)

| File | Lines | Purpose |
|------|-------|---------|
| `package_common.h` | 47 | PSI memory key, `PACKAGE_SCHEMA` constant, `Package_element_map` type alias |
| `package_common.cc` | 29 | Package module common definitions |
| `package_interface.h` | 61 | Public API: `package_context_init()`, `exist_native_proc()`, `find_native_proc_and_evoke()` |
| `package.h` | 137 | `Package` singleton container: `register_element<>()`, `lookup_element<>()`, `get_all_element<>()` |
| `package.cc` | 61 | Package singleton and destructor |
| `package_cache.cc` | 125 | Package initialization, PSI key registration, procedure registration, `exist_native_proc()`, `find_native_proc_and_evoke()` |
| `package_parse.h` | 83 | `PT_package_base`, `PT_package_proc` parse tree root classes |
| `package_parse.cc` | 51 | `PT_package_proc::make_cmd()` implementation |
| `proc.h` | 268 | `Proc` abstract class (parameters, columns, result type), `Sql_cmd_proc` abstract class (execution lifecycle), `Sql_cmd_admin_proc` |
| `proc.cc` | 185 | `Proc::PT_evoke()`, `Proc::send_result_metadata()`, `Sql_cmd_proc::execute()`, `check_access()`, `check_parameter()`, `prepare()` |

### 2.3 Demo Procedures (`sql/package/`)

| File | Lines | Purpose |
|------|-------|---------|
| `proc_dummy.h` | 172 | `Proc_dummy` (0 params, OK result), `Proc_dummy_3` (3 params, result set), `Sql_cmd_proc_dummy`, `Sql_cmd_proc_dummy_3` |
| `proc_dummy.cc` | 121 | Demo procedure implementations (debug mode only) |

### 2.4 Show Native Procedure (`sql/package/`)

| File | Lines | Purpose |
|------|-------|---------|
| `show_native_procedure.h` | 97 | `Show_native_procedure_proc`, `Sql_cmd_show_native_procedure` |
| `show_native_procedure.cc` | 202 | Implementation: lists all registered native procedures with their schemas, names, types, and parameters |

---

## 3. Modifications to Existing Files

### 3.1 `include/m_string.h`

```c++
#define C_STRING_WITH_LEN(X) ((const char *)(X)), ((sizeof(X) - 1))
```

New macro for const-char variant of `STRING_WITH_LEN`, used in column element definitions.

### 3.2 `include/my_sqlcommand.h`

```c++
enum enum_sql_command {
  ...
  SQLCOM_ADMIN_PROC,   // New command type for native procedures
  SQLCOM_END
};
```

### 3.3 `include/mysql/plugin_audit.h.pp`

Updated to include `SQLCOM_ADMIN_PROC` in the `enum_sql_command` definition, ensuring audit plugin compatibility.

### 3.4 `sql/sql_yacc.yy`

Modified the `call_stmt` rule to check for native procedures before falling through to stored procedure handling:

```yacc
call_stmt:
          CALL_SYM sp_name opt_paren_expr_list
          {
            if (($$ = im::find_native_proc_and_evoke(YYTHD, $2, $3))) {
              // Native procedure path - $$ is already set
            } else {
              $$ = NEW_PTN PT_call($2, $3);
            }
          }
        ;
```

### 3.5 `sql/sql_parse.cc`

```c++
// In init_sql_command_flags():
sql_command_flags[SQLCOM_ADMIN_PROC] = CF_AUTO_COMMIT_TRANS;

// In mysql_execute_command():
case SQLCOM_ADMIN_PROC: {
  assert(lex->m_sql_cmd != nullptr);
  res = lex->m_sql_cmd->execute(thd);
  break;
}
```

### 3.6 `sql/mysqld.cc`

```c++
#include "sql/package/package_interface.h"

// In mysqld_main(), after init_server_psi_keys():
im::package_context_init();

// In com_status_vars[]:
{"native_admin_proc",
 (char *)offsetof(System_status_var, com_stat[(uint)SQLCOM_ADMIN_PROC]),
 SHOW_LONG_STATUS, SHOW_SCOPE_ALL},
```

### 3.7 `sql/sp.cc`

```c++
#include "sql/package/package_interface.h"

// In check_routine_already_exists():
// Added check for native procedure existence for procedures (not functions)
else {
  error = thd->dd_client()->acquire<dd::Procedure>(...);
  exists = im::exist_native_proc(sp->m_db.str, sp->m_name.str);
}
if (sr == nullptr && !exists) {
  return false;  // Routine does not exist
}

// In sp_get_flags_for_command():
case SQLCOM_ADMIN_PROC:
  flags = sp_head::HAS_COMMIT_OR_ROLLBACK;
  break;
```

### 3.8 `sql/CMakeLists.txt`

```cmake
SET(SQL_SHARED_SOURCES
  ...
  common/component.cc
  package/package.cc
  package/package_cache.cc
  package/package_common.cc
  package/proc.cc
  package/package_parse.cc
  package/proc_dummy.cc
  package/show_native_procedure.cc
  ...
)
```

### 3.9 `share/messages_to_clients.txt`

Reorganized error number ranges for Huawei extensions:
- Error numbers starting at 7500: reserved for hwsql features
- Error numbers starting at 8000: new feature error messages

Added error messages:
```
start-error-number 8000

ER_ONLY_SUPPORTED_STORAGE_ENGINE_DSTORE
  eng "Only support create table with storage engine CDE."

ER_ONLY_SUPPORTED_ALTER_STORAGE_ENGINE_CDE
  eng "Do not allow to change to other storage_engine except CDE."

ER_NATIVE_PROC_PARAMETER_MISMATCH
  eng "Native procedure %s the %dth parameter mismatch"
```

---

## 4. Core Data Structures

### 4.1 Proc Class (Abstract)

The core abstract class for all native procedures:

```c++
class Proc : public PSI_memory_base {
  static constexpr unsigned int PROC_PREALLOC_SIZE = 10;

 public:
  typedef PT_package_proc PT_proc_type;
  typedef Prealloced_array<enum_field_types, PROC_PREALLOC_SIZE> Parameters;

  typedef struct st_column_element {
    enum enum_field_types type;
    const char *name;
    std::size_t name_len;
    std::size_t size;
  } Column_element;

  typedef Prealloced_array<Column_element, PROC_PREALLOC_SIZE> Columns;

  enum class Result_type {
    RESULT_NONE,    // Initial state
    RESULT_OK,      // Only OK or ERROR protocol packet
    RESULT_SET      // Send result set with columns and rows
  };

 public:
  explicit Proc(PSI_memory_key key);
  virtual ~Proc() {}

  // Generate parse tree root for this procedure
  Parse_tree_root *PT_evoke(THD *thd, PT_item_list *pt_expr_list,
                             const Proc *proc) const;

  // Create the Sql_cmd object for execution (must be implemented by subclass)
  virtual Sql_cmd *evoke_cmd(THD *thd, mem_root_deque<Item *> *list) const = 0;

  Result_type get_result_type() const { return m_result_type; }
  const Parameters *get_parameters() const { return &m_parameters; }
  const Columns &get_columns() const { return m_columns; }

  // Send column metadata for RESULT_SET procedures
  bool send_result_metadata(THD *thd) const;

  virtual const std::string str() const = 0;     // Procedure name
  virtual const std::string qname() const {       // Qualified name
    std::stringstream ss;
    ss << PACKAGE_SCHEMA << "." << str();
    return ss.str();
  }

 protected:
  Result_type m_result_type;
  Parameters m_parameters;   // Parameter types
  Columns m_columns;         // Result set column definitions
};
```

### 4.2 Sql_cmd_proc Class (Abstract)

The execution lifecycle class for native procedures:

```c++
class Sql_cmd_proc : public Sql_cmd {
 protected:
  enum class Priv_type { PRIV_NONE_ACL = 0, PRIV_SUPER_ACL };

 public:
  explicit Sql_cmd_proc(THD *thd, mem_root_deque<Item *> *list,
                        const Proc *proc, Priv_type priv_type);

  virtual bool execute(THD *thd) override;       // Full execution lifecycle
  virtual bool pc_execute(THD *thd) = 0;          // Must be implemented by subclass
  virtual void send_result(THD *thd, bool error);  // Default: OK/ERROR packet
  virtual bool check_access(THD *thd);             // Default: SUPER_ACL check
  virtual bool check_parameter();                   // Parameter type/count validation
  virtual bool prepare(THD *thd) override;         // Calls check_access + check_parameter

 protected:
  THD *m_thd;
  mem_root_deque<Item *> *m_list;   // Input parameters
  const Proc *m_proc;               // Procedure definition
  Priv_type m_priv_type;            // Required privilege level
};
```

### 4.3 Sql_cmd_admin_proc Class

Convenience base class for administrative procedures requiring SUPER_ACL:

```c++
class Sql_cmd_admin_proc : public Sql_cmd_proc {
 public:
  explicit Sql_cmd_admin_proc(THD *thd, mem_root_deque<Item *> *list,
                              const Proc *proc)
      : Sql_cmd_proc(thd, list, proc, Priv_type::PRIV_SUPER_ACL) {}

  virtual enum_sql_command sql_command_code() const override {
    return SQLCOM_ADMIN_PROC;
  }
};
```

### 4.4 Package Class (Singleton Container)

```c++
class Package : public PSI_memory_base {
  typedef Package_element_map<Proc> Proc_map;

  template <typename T>
  struct Type_selector {};

 public:
  explicit Package(PSI_memory_key key);
  static Package *instance();

  template <typename T>
  bool register_element(const std::string &schema_name,
                        const std::string &element_name, T *element);

  template <typename T>
  const T *lookup_element(const std::string &schema_name,
                          const std::string &element_name);

  template <typename T>
  const Proc_map *get_all_element();

 private:
  Proc_map m_proc_map;    // (schema, name) -> Proc* map
};
```

### 4.5 PT_package_proc Class (Parse Tree Root)

```c++
class PT_package_base : public Parse_tree_root {
 public:
  explicit PT_package_base() {}
  virtual ~PT_package_base() = 0;
};

class PT_package_proc final : public PT_package_base {
 public:
  explicit PT_package_proc(PT_item_list *opt_expr_list, const Proc *proc)
      : m_opt_expr_list(opt_expr_list), m_proc(proc) {}

  Sql_cmd *make_cmd(THD *thd) override;

 private:
  PT_item_list *m_opt_expr_list;
  const Proc *m_proc;
};
```

### 4.6 PSI_memory_base and Disable_copy_base

```c++
class PSI_memory_base {
 public:
  PSI_memory_base(PSI_memory_key key) : m_key(key) {}
  virtual ~PSI_memory_base() {}
  PSI_memory_key psi_key() { return m_key; }
  void set_psi_key(PSI_memory_key key) { m_key = key; }
 private:
  PSI_memory_key m_key;
};

class Disable_copy_base {
 public:
  Disable_copy_base() {}
  virtual ~Disable_copy_base() {}
 private:
  Disable_copy_base(const Disable_copy_base &);
  Disable_copy_base(const Disable_copy_base &&);
  Disable_copy_base &operator=(const Disable_copy_base &);
};
```

### 4.7 Package_element_map Type

```c++
template <typename T>
using Package_element_map = Pair_key_unordered_map<std::string, std::string, T>;
```

This is an unordered_map with a case-insensitive pair key (schema_name, element_name).

---

## 5. Execution Flow

### 5.1 System Initialization

```
mysqld_main():
  init_server_psi_keys()
  im::package_context_init()
    |
    +-- init_package_psi_key()     // Register PSI memory key
    +-- package_inited = true
    +-- Register all native procedures:
        |
        +-- #ifndef NDEBUG (Debug only):
        |   register_package<Proc, Proc_dummy>("mysql")
        |   register_package<Proc, Proc_dummy_3>("mysql")
        |
        +-- register_package<Proc, Show_native_procedure_proc>("dbms_admin")
```

### 5.2 Procedure Call Execution

```
User: CALL schema.proc_name(arg1, arg2, ...)

  1. Parser (sql_yacc.yy):
     - Recognizes CALL statement with sp_name and opt_paren_expr_list
     - Calls im::find_native_proc_and_evoke(YYTHD, sp_name, pt_expr_list)
       -> Package::lookup_element<Proc>(schema, name)
       -> If found: Proc::PT_evoke(thd, pt_expr_list, proc)
         -> new (thd->mem_root) PT_package_proc(pt_expr_list, proc)
       -> If not found: returns nullptr, falls through to PT_call (regular SP)

  2. Parse tree execution:
     PT_package_proc::make_cmd(thd)
       a. Contextualize expression list (evaluate arguments)
       b. Proc::evoke_cmd(thd, proc_args)
          -> new (thd->mem_root) Sql_cmd_type(thd, list, this)
       c. Set lex->sql_command = SQLCOM_ADMIN_PROC
       d. Return Sql_cmd

  3. mysql_execute_command():
     case SQLCOM_ADMIN_PROC:
       res = lex->m_sql_cmd->execute(thd)

  4. Sql_cmd_proc::execute(thd):
     a. prepare(thd):
        - check_access(thd):
          PRIV_NONE_ACL: allow all
          PRIV_SUPER_ACL: require SUPER privilege
        - check_parameter():
          - Verify argument count matches definition
          - Verify each argument's data_type matches parameter definition
          - If mismatch: ER_NATIVE_PROC_PARAMETER_MISMATCH
        - set_prepared()
     b. pc_execute(thd):
        - Subclass-specific execution logic
     c. send_result(thd, error):
        RESULT_OK: my_ok(thd) or error already set
        RESULT_SET: subclass overrides send_result()

  5. send_result() for RESULT_SET procedures:
     a. Proc::send_result_metadata(thd):
        - Create Item for each column definition (LONGLONG, VARCHAR, NEWDECIMAL)
        - Send column metadata via Protocol::send_result_metadata()
     b. Protocol::start_row() / store_xxx() / end_row() for each result row
     c. my_eof(thd)
```

### 5.3 Native vs Stored Procedure Resolution

```
CALL schema.proc_name(args):
  |
  +-- exist_native_proc(schema, name)?
  |     YES --> Native procedure path (PT_package_proc)
  |     NO  --> Regular stored procedure path (existing MySQL code)
  |
  +-- Native procedure cannot be:
      - Created with CREATE PROCEDURE (ER_SP_ALREADY_EXISTS)
      - Dropped with DROP PROCEDURE (ER_SP_DOES_NOT_EXIST)
      - Called from stored functions or triggers (implicit commit check)
```

### 5.4 Parameter Checking

```c++
bool Sql_cmd_proc::check_parameter() {
  std::size_t actual_size = (m_list == nullptr ? 0 : m_list->size());
  std::size_t define_size = m_proc->get_parameters()->size();

  // Check parameter count
  if (actual_size != define_size) {
    my_error(ER_SP_WRONG_NO_OF_ARGS, MYF(0), "PROCEDURE",
             m_proc->qname().c_str(), define_size, actual_size);
    return true;
  }

  // Check parameter types
  if (actual_size > 0) {
    std::size_t i = 0;
    for (auto item = m_list->begin(); item != m_list->end(); ++item) {
      if ((*item)->data_type() != m_proc->get_parameters()->at(i)) {
        my_error(ER_NATIVE_PROC_PARAMETER_MISMATCH, MYF(0),
                 m_proc->qname().c_str(), i + 1);
        return true;
      }
      i++;
    }
  }
  return false;
}
```

### 5.5 Result Metadata Sending

```c++
bool Proc::send_result_metadata(THD *thd) const {
  mem_root_deque<Item *> field_list(thd->mem_root);
  Item *item;

  for (auto it = m_columns.begin(); it != m_columns.end(); it++) {
    switch ((*it).type) {
      case MYSQL_TYPE_LONGLONG:
        field_list.push_back(
            item = new Item_int(Name_string((*it).name, (*it).name_len),
                                (*it).size, MY_INT64_NUM_DECIMAL_DIGITS));
        item->set_nullable(true);
        break;
      case MYSQL_TYPE_VARCHAR:
        field_list.push_back(item =
                                 new Item_empty_string((*it).name, (*it).size));
        item->set_nullable(true);
        break;
      case MYSQL_TYPE_NEWDECIMAL:
        field_list.push_back(item = new Item_decimal((longlong)0, false));
        item->set_nullable(true);
        item->item_name.copy((*it).name, (*it).name_len);
        break;
      default:
        DBUG_ASSERT(0);
    }
  }
  return thd->send_result_metadata(field_list,
                                   Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF);
}
```

### 5.6 Show Native Procedure Execution

```
CALL dbms_admin.show_native_procedure():

  1. Sql_cmd_show_native_procedure::pc_execute() -- returns false (no-op)
  2. Sql_cmd_show_native_procedure::send_result():
     a. Proc::send_result_metadata() -- sends 4 columns:
        SCHEMA_NAME (VARCHAR 128), PROC_NAME (VARCHAR 128),
        PROC_TYPE (VARCHAR 128), PARAMETERS (VARCHAR 1024)
     b. Iterate all registered procs (sorted by schema+name):
        - Evoke Sql_cmd to get sql_command_code -> PROC_TYPE string
        - Build PARAMETERS string from field type enum names
        - Send row via Protocol
     c. my_eof(thd)
```

---

## 6. System Variables

No dedicated system variables for the Native Package Framework itself. Individual native procedures may define their own variables as needed.

Status variable added:

| Variable | Type | Description |
|----------|------|-------------|
| `Com_native_admin_proc` | SHOW_LONG_STATUS | Count of native admin procedure calls |

---

## 7. EXPLAIN Output

Not applicable. Native procedures are executed via CALL statements, not as part of SELECT queries. The framework does not participate in EXPLAIN output.

---

## 8. Error Handling

### Error Codes

| Error Code | Name | Message |
|-----------|------|---------|
| 8001 | ER_NATIVE_PROC_PARAMETER_MISMATCH | "Native procedure %s the %dth parameter mismatch" |
| (existing) | ER_SP_ALREADY_EXISTS | "PROCEDURE %s already exists" (when trying to CREATE a native proc) |
| (existing) | ER_SP_DOES_NOT_EXIST | "PROCEDURE %s does not exist" (when trying to DROP a native proc) |
| (existing) | ER_SP_WRONG_NO_OF_ARGS | "Incorrect number of arguments for PROCEDURE %s; expected %d, got %d" |
| (existing) | ER_SPECIFIC_ACCESS_DENIED_ERROR | "Access denied; you need (at least one of) the SUPER privilege(s) for this operation" |
| (existing) | ER_COMMIT_NOT_ALLOWED_IN_SF_OR_TRG | "Explicit or implicit commit is not allowed in stored function or trigger" |

### Stored Function/Trigger Restriction

Native procedures cannot be called from stored functions or triggers because `SQLCOM_ADMIN_PROC` is flagged with `CF_AUTO_COMMIT_TRANS`, which maps to `sp_head::HAS_COMMIT_OR_ROLLBACK`. The existing MySQL check for implicit commit detection handles this:

```
CREATE FUNCTION func() RETURNS INT
BEGIN
  CALL mysql.dummy();  -- ERROR: Explicit or implicit commit is not allowed
  RETURN 0;
END|
```

### Error Propagation

- If `pc_execute()` returns true, `send_result()` is called with `error=true` and the error is already set on THD
- If `check_access()` or `check_parameter()` fails, execution is skipped and the error is sent immediately
- Native procedures use the standard MySQL error reporting mechanism (`my_error()`)

### Error Number Reorganization

Error numbers for Huawei extensions were moved to the 7500-8000+ range to avoid conflicts with community MySQL error numbers:
- 5000-5999: reserved section
- 7500-7999: hwsql server-to-client messages
- 8000+: new feature error messages (ER_ONLY_SUPPORTED_STORAGE_ENGINE_DSTORE, ER_ONLY_SUPPORTED_ALTER_STORAGE_ENGINE_CDE, ER_NATIVE_PROC_PARAMETER_MISMATCH)

---

## 9. Test Coverage

MTR test cases in `mysql-test/suite/rds/`:

| Test File | Result File | Description |
|-----------|-------------|-------------|
| `t/native_procedure.test` | `r/native_procedure.result` | Core functionality: CALL, parameter matching, privilege checking, conflict with stored procedures |
| `t/native_procedure_bugfix.test` | `r/native_procedure_bugfix.result` | Bug fix: native procedures cannot be called from stored functions or triggers |
| `t/show_native_procedure_debug.test` | `r/show_native_procedure_debug.result` | Debug mode: shows all registered procedures including dummy procs |
| `t/show_native_procedure_release.test` | `r/show_native_procedure_release.result` | Release mode: shows only production procedures |

### Key Test Scenarios

1. **Basic CALL**: `CALL mysql.dummy()` -- zero-argument procedure with OK result
2. **Parameterized CALL**: `CALL mysql.dummy_3(1, 'xpchild', 99.99)` -- three-argument procedure with result set
3. **Parameter type mismatch**: `CALL mysql.dummy_3(1, 1, 1)` -- second arg should be VARCHAR, not LONGLONG
4. **NULL parameters**: `CALL mysql.dummy_3(NULL, 'xpchild', 99.99)` -- NULL not allowed for LONGLONG param
5. **Wrong argument count**: `CALL mysql.dummy(1)` -- expected 0, got 1
6. **CREATE PROCEDURE conflict**: `CREATE PROCEDURE mysql.dummy() ...` -- ER_SP_ALREADY_EXISTS
7. **DROP PROCEDURE conflict**: `DROP PROCEDURE mysql.dummy` -- ER_SP_DOES_NOT_EXIST
8. **Privilege check**: Non-SUPER user calling admin procedure -- ER_SPECIFIC_ACCESS_DENIED_ERROR
9. **Stored function restriction**: Native proc in function body -- ER_COMMIT_NOT_ALLOWED_IN_SF_OR_TRG
10. **Trigger restriction**: Native proc in trigger body -- ER_COMMIT_NOT_ALLOWED_IN_SF_OR_TRG
11. **show_native_procedure**: Lists all registered native procedures with their schemas, names, types, and parameter types
12. **show_native_procedure wrong args**: `CALL dbms_admin.show_native_procedure(1)` -- ER_SP_WRONG_NO_OF_ARGS

---

## 10. Limitations

- Only procedures are supported (no native functions)
- Parameter types are limited to: `MYSQL_TYPE_LONGLONG`, `MYSQL_TYPE_VARCHAR`, `MYSQL_TYPE_NEWDECIMAL`
- No support for OUT/INOUT parameters
- No support for OVERLOAD (multiple procedures with same name but different parameter counts)
- Procedure registration is static (at server startup); no dynamic registration/unregistration
- All native procedures share `SQLCOM_ADMIN_PROC` as their SQL command type, which limits query logging granularity
- The `Package` singleton is not thread-safe for registration (registration happens during single-threaded initialization only)
- Case-insensitive schema and procedure name matching via `Pair_key_comparator`
- `Proc_dummy` and `Proc_dummy_3` are only available in debug builds (`#ifndef NDEBUG`)
- `show_native_procedure` reveals different procedures in debug vs release mode

---

## 11. Implementation Order

Recommended order for implementing this feature on a clean codebase:

1. **Common infrastructure** (`sql/common/component.h/.cc`) -- Base classes, Pair_key_unordered_map, string utilities
2. **Package common** (`sql/package/package_common.h/.cc`) -- PSI key, PACKAGE_SCHEMA constant
3. **Proc abstract class** (`sql/package/proc.h/.cc`) -- Procedure definition interface, Sql_cmd_proc execution lifecycle
4. **Package container** (`sql/package/package.h/.cc`) -- Singleton with register/lookup
5. **Parse tree integration** (`sql/package/package_parse.h/.cc`) -- PT_package_proc
6. **Package interface** (`sql/package/package_interface.h`) -- Public API declaration
7. **Package cache/registration** (`sql/package/package_cache.cc`) -- package_context_init() with all registrations
8. **SQL command type** (`include/my_sqlcommand.h`) -- SQLCOM_ADMIN_PROC
9. **Parser integration** (`sql/sql_yacc.yy`) -- CALL statement native proc check
10. **SQL execution integration** (`sql/sql_parse.cc`) -- SQLCOM_ADMIN_PROC dispatch, CF_AUTO_COMMIT_TRANS flag
11. **Stored procedure conflict** (`sql/sp.cc`) -- Prevent CREATE/DROP of native proc names, HAS_COMMIT_OR_ROLLBACK flag
12. **Server initialization** (`sql/mysqld.cc`) -- Call package_context_init(), add Com_native_admin_proc status var
13. **Build integration** (`sql/CMakeLists.txt`) -- Add source files
14. **Error messages** (`share/messages_to_clients.txt`) -- ER_NATIVE_PROC_PARAMETER_MISMATCH, error number reorganization
15. **Demo procedures** (`sql/package/proc_dummy.h/.cc`) -- Proc_dummy and Proc_dummy_3
16. **Show native procedure** (`sql/package/show_native_procedure.h/.cc`) -- Introspection utility
17. **Test suite** -- MTR test cases
