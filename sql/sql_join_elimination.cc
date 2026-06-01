/* Copyright (c) 2025, Huawei and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
  @page PAGE_JOIN_ELIMINATION Join Elimination

  @section INTRODUCTION Introduction

  Join elimination is a feature found in prominent DBMSs.

  Its principle is that, sometimes, a table can be removed from a join without
  changing the query's result.

  There exists inner join elimination and outer (=left) join elimination.

  We implement only left-join elimination (LJE). Because inner-join elimination
  is more complex and has not been requested by our customers (but we discuss
  its principle a bit further down, after LJE).

  Our LJE is triggered in three scenarios, if the optimizer_switch flag
  "left_join_elimination" is on.

  @section GROUP_BY_OR_DISTINCT Elimination due to GROUP BY or DISTINCT

  Examples:

  SELECT T1.A FROM T1 LEFT JOIN T2 ON T2.A>T1.B GROUP BY T1.A;
  SELECT DISTINCT T1.A FROM T1 LEFT JOIN T2 ON T2.A>T1.B;

  The left join guarantees that each row of T1 is present N times in the
  result of the LEFT JOIN, with N>=1. T2 is not referenced anywhere in the
  query except in the left join's condition. So the presence of T2 only causes
  a "multiplication" of the rows in the result of the FROM clause; it
  influences the value of N but does not change that N>=1 holds. As we have
  GROUP BY, which only does duplicate elimination (no aggregation functions
  are present), the "multiplication" is canceled. So these queries can be
  rewritten to:

  SELECT T1.A FROM T1 GROUP BY T1.A;
  SELECT DISTINCT T1.A FROM T1;

  Aggregate and window functions, which depend on the cardinality, most often
  make this elimination impossible.

  @section UNIQUE_COLUMNS Elimination due to unique columns

  Examples:

  SELECT T1.A FROM T1 LEFT JOIN T2 ON T2.UNIQUE_COL=T1.B;

  Using the notation of the previous section, here we know that N=1 (no
  multiplication). So the query is equivalent to:

  SELECT T1.A FROM T1;

  There is a variant of this, where a column is "actually unique" even though
  not declared as such. If T2 is a derived table with a definition like this:

  (SELECT expr1, expr2, [aggregates], ... FROM ... GROUP BY expr1, expr2)
    AS T2(col1,col2)

  then we can say that the columns (col1,col2) together form a sort of "unique
  key" of T2: even though there is no index for it, it is true that for a
  given value of col1 and of col2, there is only one row in T2. So in this
  query:

  SELECT T1.A FROM T1 LEFT JOIN (...) AS T2
    ON T2.col1=T1.B AND T2.col2=T1.C;

  T2 can be eliminated.

  It is also the case if T2 has a definition like this:

   (SELECT DISTINCT expr1, expr2 FROM ...)
    AS T2(col1,col2)

  because again we can say that the columns (col1,col2) together form a sort of
  "unique key" of T2.

  For an example of how such superfluous joins can appear in real-life,
  well-intended queries, please search for "person" in mtr test
  join_elimination.test.

  @section SEMI_JOIN Elimination due to semi-join and anti-join

  If the left join is inside a semi-join or anti-join nest, then elimination
  in the left join is possible: because the left join guarantees N>=1 and the
  output of the semi-join or anti-join is identical if N is 1 or if N is more.
  Even if there are several nest levels between the semi-anti-join and the
  left join.

  T1 SEMI-JOIN (T2 LEFT JOIN T3 ON cond23) ON cond123
  If cond123 does not reference T3, is equivalent to
  T1 SEMI-JOIN T2 ON cond123

  This also applies if the left join is part of an EXISTS or IN subquery
  (which has not been transformed to semi-join or anti-join for some reason),
  with one more constraint: we need to make sure that no part of the subquery
  depends on the cardinality. Such part can be an aggregate or window
  function. So we exclude these.

  EXISTS(SELECT 1 FROM T2 LEFT JOIN T3 ON cond23 WHERE where_cond)
  If where_cond does not reference T3, is equivalent to
  EXISTS(SELECT 1 FROM T2 WHERE where_cond)

  @section SUPPORTED_STATEMENTS Supported statements

  Join elimination is done in:
  - SELECT
  - {INSERT,REPLACE} SELECT
  - CREATE SELECT
  - any subquery of any statement (f.ex. INSERT VALUES((subq))).

  And not for anything else. Single-table UPDATE-DELETE and INSERT VALUES have
  no join. Multi-table UPDATE-DELETE has a join, so in theory we could do
  elimination there. We would need to gather table references found in the
  UPDATE or DELETE clause, but there is more, because the implementation of
  multi-table UPDATE-DELETE establishes pointers between tables to modify and
  tables in FROM, so there is risk if we eliminate a table in FROM. Also, the
  performance gain of elimination is expected to occur mostly for GROUP BY or
  DISTINCT (where the cardinality can be greatly reduced), rather than for
  UNIQUE. And these two statements cannot contain GROUP BY or DISTINCT. So, we
  think that adding support for them is not worth the potential gain.

  @section IMPLEMENTATION_SUMMARY Implementation summary

  Entry point: do_join_elimination().

  We do an analysis on the join nests, to find tables in left joins which are
  not referenced outside of the join, with equalities on UNIQUE or with GROUP
  BY or DISTINCT or within semi- and anti-joins or certain sub-queries. The
  eliminated table is removed from Query_block's m_table_nest (so that the
  rewritten query printed by EXPLAIN does not show it) and leaf_tables (so
  that JOIN::optimize() ignores it), but not removed from Query_block's
  m_table_list (for cleanup-related reasons, see further down), and not
  removed from LEX::query_tables (which implies that they will be opened at
  the next execution, if in a prepared statement).

  @section IMPLEMENTATION_CALL_ORDER Order of calls in the implementation

  By definition, join elimination and simplify_joins() are rather independent:
  the latter is mostly about changing
  (T1 LEFT JOIN T2 ON cond) WHERE cond_on_T2 ...
  to
  T1, T2 WHERE cond AND cond_on_T2
  if cond_on_T2 is NULL-rejecting; and such LEFT JOIN is not a candidate for
  elimination because T2 is referenced from outside of the join nest (by
  cond_on_T2).

  So there is no strong conceptual reason to execute one module before the
  other. But there are implementation reasons:
  - by doing simplify_joins first, elimination has a simpler join nest
  structure to examine, and can look at more information (e.g.
  nested_join->used_tables).
  - however, elimination must then correct this information (removing bits out
  of maps).

  Another similar question is the order of calling join elimination and
  record_join_nest_info(). By calling record_join_nest_info() first, we have
  the value of this->outer_join available when starting join elimination. If
  this value is zero we can skip calling do_join_elimination(), which is a
  possibly costly function as it has a recursive traversal of the FROM clause.
  This way, we ensure that the CPU cost of the join elimination feature for a
  query without outer joins (the most common case) is zero. The accepted
  drawback is that if a table is eliminated, elimination must then correct
  information previously set by record_join_nest_info().

  @section IMPLEMENTATION_TABLENO_GAP Solving the problem of gaps in the
  values of Table_ref::tableno().

  Imagine we have a query over 3 tables A-B-C, with tableno 0,1,2 and maps
  1,2,4. Imagine that join elimination removed B. We now have two tables A-C,
  tableno 0,2, maps 1,4. leaf_table_count is 2, thus JOIN::tables will be set
  to 2 when optimization starts.

  As a consequence, some invariants which used to be true, are now changed,
  and code outside of join elimination is adapted to the new rules:

  - the assumption that the maximum value of tableno is leaf_table_count-1, is
  now false. This maximum can be bigger.

  - in the example above, if we need to add a new table (f.ex. a tmp table for
  a semijoin-materialization nest), we must not give it tableno 2, which is
  already taken by table C ; we should instead give it tableno 3. To this aim,
  we remember the maximum tableno value ever assigned, in
  Query_block::m_hwm_free_tableno.

  - the phase of optimization (after init_planner_arrays() and before
  get_best_combination()) which assumes that tableno X can be found at
  JOIN::join_tab[X], must not do so anymore. In our example, tableno 2 is at
  index 1; trying index 2 would do a buffer overrun. That is why
  optimize_keyuse() is changed.

  Instead of accepting this gap in the sequence of tableno-s and fixing code
  which assumed no gaps, I tried to instead eliminate the gap by renumbering
  tables which remain after elimination: giving them contiguous tableno, and
  thus also changing maps. In our example, table A would keep number 0, table
  C would switch from number 2 to 1. But I cannot use remap_tables() as we are
  much later than view merging: we are after fix_fields(), maps have
  propagated in lots of places like all Item's used_tables(). And we need this
  propagation in join elimination btw, as we rely on used_tables(). Cannot use
  Query_block::update_used_tables() either: well we could but it does not
  recalculate everything: f.ex. tables not involved in the left join (like C)
  have to change their map, and they may be referenced from a lateral table,
  so we need to update m_lateral_deps in any lateral table; also have to
  update any nested_join's maps; so, not impossible but significant work, and
  risky as our changes can extend to the whole query.

  @section IMPLEMENTATION_DEFAULT Solving the problem of DEFAULT(column).

  https://dev.mysql.com/doc/refman/8.0/en/miscellaneous-functions.html#function_default

  Considering:
  CREATE TABLE t0 (c1 TINYINT);
  INSERT INTO t0 VALUES(0),(1);
  CREATE TABLE t98 (c3 DECIMAL(10,0) NOT NULL DEFAULT '1317787993');
  (necessary details: declared NOT NULL and with a default).

  We expect DEFAULT(t98.c3) to return 1317787993.
  This is generally the case, but not for:

  SELECT DEFAULT(t98.c3) FROM t0 LEFT JOIN t98 ON TRUE;

  In Item_default_value::fix_fields(), we make a Field and point it to
  the default value (s->default_values). During execution, we have
  NULL-complementing for t98, so we fill table->record[0] with NULLs and
  call t98->set_nullable(); then we evaluate DEFAULT, which calls
  Item_field::send(), which tests Field:is_null() which returns true
  because, when the field is not nullable (like here),
  table->is_nullable() is used :

  Field::is_null() {
      if (is_nullable()) return (m_null_ptr[row_offset] & null_bit);
      if (is_tmp_nullable()) return m_is_tmp_null;
      return table->has_null_row();  <- HERE
  }

  (if the field had been nullable, the first if() would trigger, and as
  m_null_ptr points into the default values, which are not NULL,
  is_null() would have returned false). We can say that DEFAULT is
  isolated from the current row of the table by having a different
  record pointer, but alas not totally isolated as both share the
  'table' pointer and thus share the value of table->has_null_row().

  So the SELECT returns NULL. This is a bug in MySQL, which we filed as
  https://bugs.mysql.com/bug.php?id=119453 . (If the field had been nullable,
  the SELECT would have returned 1317787993.)

  Let's now add GROUP BY 1 to our query:
  SELECT DEFAULT(t98.c3) FROM t0 LEFT JOIN t98 ON TRUE GROUP BY 1;

  Then left join elimination is tried on this query ;
  Item_default_value::used_tables() is always 0 (which is actually sensible as
  it's not really a reference: DEFAULT is not going to need any row of the
  table, only return a default found in the table's definition), so this
  reference is ignored when we wonder if we can eliminate the table, so we do
  elimination, and during elimination we do:
  bitmap_clear_all(tl->table->read_set). However, this breaks for our query:
  this Item's evaluation, because the item is not NULL-complemented anymore,
  continues into Field_new_decimal::val_decimal() which has
  ASSERT_COLUMN_MARKED_FOR_READ (in Item_default_value::fix_fields() the
  column was added to read_set but join elimination cleared it afterwards).
  The assertion fires.

  If we removed the read_set clearing, Item_default_value would return
  1317787993.

  Because we would like to keep our read_set-clearing, and we prefer to not
  have a result difference between without- and with-elimination, and fixing
  the MySQL bug is not doable with short and clean code, we chose to avoid the
  problem: if a query contains DEFAULT(X) where X is a column of a table on
  the right side of a left join, we do not do elimination.

  @section INNER_JOIN_ELIMINATION Principle of inner join elimination

  As explained in the previous paragraphs, the key point which makes left join
  elimination work is the guarantee that N>=1 (sufficient when DISTINCT or
  GROUP BY are present, or within anti-semi-join or certain sub-queries), or
  N=1 (guaranteed by a UNIQUE key) for when these characteristics (DISTINCT
  etc) are not present.

  There are cases where these guarantees hold for an inner join too. The first
  case is the self-join: if we have a join of a table with itself, with only
  equality conditions on the same column:

  SELECT T1.A FROM T1 JOIN T1 AS T2 ON T2.B=T1.B

  (T1 and T2 are the same table, both B are the same column). In this query,
  N>=1. Except if T1.B is NULL, and to handle this special case, we can
  add IS NOT NULL. Having N>=1 then allows us to do elimination if DISTINCT or
  GROUP BY are there:

  SELECT DISTINCT T1.A FROM T1 JOIN T1 AS T2 ON T2.B=T1.B
  becomes
  SELECT DISTINCT T1.A FROM T1 WHERE T1.B IS NOT NULL

  (Digression : even if T2.B is referenced in the SELECT list we can
  eliminate,
  SELECT DISTINCT T1.A,T2.B FROM T1 JOIN T1 AS T2 ON T2.B=T1.B
  becomes
  SELECT DISTINCT T1.A,T1.B FROM T1 WHERE T1.B IS NOT NULL).

  (Digression : if we have a semi-join instead of an inner join,
  SELECT T1.A FROM T1 SEMIJOIN T1 AS T2 ON T2.B=T1.B
  then N=1, DISTINCT is not a necessary ingredient and we can rewrite to:
  SELECT T1.A FROM T1 WHERE T1.B IS NOT NULL).

  An extreme, unrealistic sub-case of this is when ON is just testing a
  constant and TRUE condition.

  Another sub-case is when the column in ON (B) is UNIQUE, then N=1 (and
  DISTINCT is thus not a necessary ingredient for elimination); and we
  can eliminate even if any column of T2 is referenced in the SELECT list,
  SELECT T1.A,T2.C FROM T1 JOIN T1 AS T2 ON T2.B=T1.B
  becomes
  SELECT T1.A,T1.C FROM T1 WHERE T1.B IS NOT NULL).

  The second case is FK-PK relationship (or FK-{UNIQUE key}), with FK declared
  in the table's definition.

  SELECT T1.A FROM T1 JOIN T2 ON T1.FK=T2.PK;

  where T1.FK, is a declared FOREIGN KEY referencing T2.PK which is a PRIMARY
  (or UNIQUE) key of T2. In this query, N=1. Except if T1.FK is NULL. So we
  can eliminate and rewrite to:

  SELECT T1.A FROM T1 WHERE T1.FK IS NOT NULL;

  However, there is a difficulty: if the FOREIGN_KEY_CHECKS variable has been
  set to off in the past, and updates have been done to tables then, we can
  not be sure that the FK-PK relationship really holds in the data at later
  points in time.

  Note that unlike in the LEFT JOIN cases, it's not possible to accept other
  AND-ed irrelevant conditions in ON, as they may make N go down to 0. Like in

  SELECT T1.A FROM T1 JOIN T2 ON T1.FK=T2.PK AND T2.OTHER_COL>T1.YET_OTHER_COL
*/

#include <algorithm>  // std::transform
#include "my_bit.h"   // my_count_bits
#include "sql/handler.h"
#include "sql/nested_join.h"  // NESTED_JOIN
#include "sql/opt_trace.h"
#include "sql/sql_executor.h"  // unwrap_rollup_group
#include "sql/sql_insert.h"
#include "sql/sql_lex.h"
#include "sql/table_function.h"  // Table_function::used_tables
#include "sql/window.h"          // Window::first_partition_by

namespace {

/** This object is used to analyze the ON condition, looking for equalities
which may impose values to columns of a UNIQUE key on the weak side of the
join. It is inspired by Group_check of aggregate_check.cc, and we
intentionally re-used the names for variables and functions. Terminology
(from the SQL Standard): in "table1 LEFT JOIN table2", table1 is called
the strong side, and table2 is called the weak side. */

class Unique_analyzer {
 private:
  /// Query block owning the join.
  const Query_block *qb;
  /// Map of all tables included in the weak side.
  table_map weak_tables;
  /// Map of weak-side tables for which we have already run the unique-key
  /// search and needn't do it again.
  table_map tested_map_for_keys{0};
  /// Map of weak-side tables for which we have found that all columns of a
  /// unique key are functionally dependent: the "whole row" of this table is
  /// entirely determined by the row on the strong side.
  table_map whole_tables_fd{0};
  /// List of columns of weak-side tables which are Functionally Dependent
  /// ("fd") on columns of strong-side tables: given one row from the strong
  /// side, at most one row in the weak side may match. The value of such
  /// weak-side column is entirely determined by the row on the strong side.
  Mem_root_array<const Item_ident *> fd;
  void analyze_scalar_eq(const Item_func_eq *cond, const Item *left_item,
                         const Item *right_item);

 public:
  Unique_analyzer(const Query_block *qb_, table_map weak_tables_,
                  MEM_ROOT *mem_root_)
      : qb(qb_), weak_tables(weak_tables_), fd(mem_root_) {}
  void analyze_conjunct(const Item *conjunct);
  bool all_weak_side_is_fd();
};

void Unique_analyzer::analyze_scalar_eq(const Item_func_eq *cond,
                                        const Item *left_item,
                                        const Item *right_item) {
  table_map left_tables = left_item->used_tables();
  table_map right_tables = right_item->used_tables();
  bool left_is_column = left_item->local_column(qb).is_true();
  bool right_is_column = right_item->local_column(qb).is_true();
  // We look for expression_made_of_strong_side=column_of_weak_side
  if (right_is_column && (weak_tables & right_tables) &&
      !(weak_tables & left_tables)) {
  } else if (left_is_column && (weak_tables & left_tables) &&
             !(weak_tables & right_tables)) {
    // arguments are in reverse order, swap them
    std::swap(left_item, right_item);
    std::swap(left_tables, right_tables);
    std::swap(left_is_column, right_is_column);
  } else
    return;  // this equality brings nothing

  /* Prevent table1 LEFT JOIN table2 ON RAND()=table2.A : for one row of
  table1, if MySQL chooses to do a scan of table2 (as opposed to an EQ_REF
  lookup), there could be multiple evaluations of RAND() and thus multiple
  matches in table2, even though table2.A is unique ; so elimination would not
  be possible. This is different from the case where the left expression in
  the equality is a deterministic expression of columns of table1, or is a
  constant. */
  if (left_tables & RAND_TABLE_BIT) return;

  /* Given that our goal is, in the end, to identify base table unique
  columns, a view column wrapping a non-column expression is of no interest to
  us. */
  right_item = right_item->real_item();
  if (right_item->type() != Item::FIELD_ITEM) return;

  auto left_type = left_item->data_type();
  auto right_type = right_item->data_type();

  /* If the weak-side argument is a string, check that it is compared as
  string. This is to guard against :
  table1 LEFT JOIN table2 ON table1.num_col=table2.char_col;
  indeed in this case, MySQL compares as floating-point, so if we have 1 in
  num_col, and have "1" and "1.0" in char_col (respecting uniqueness of
  strings), both rows in table2 will match the row in table1. While we want a
  max cardinality of 1 in the join operation. Moreover, even if comparing two
  strings, it should be done with the collation of the weak side's column.
  Otherwise, table2's char_col could contain "e" and "é" and have an
  accent-sensitive collation (respecting uniqueness), and both would match a
  string "e" on the strong side, if the comparison collation was
  accent-insensitive. Or the problem could come from space-padding.

  The danger is only when the weak-side column is of type string. Other
  types do not experience such phenomenon of two values yielding one after
  conversion. For example, integer 1 has a single string representation,
  integer 20000101 has a single DATE representation, an ENUM value has a
  single integer or string representation... */

  if (!is_string_type(right_type) ||
      (is_string_type(left_type) &&
       cond->compare_collation() == right_item->collation.collation))
    fd.push_back(down_cast<const Item_ident *>(right_item));
  /* Note that we do not do equality propagation inside the ON condition. It's
  unlikely to help in real life. */
}

void Unique_analyzer::analyze_conjunct(const Item *conjunct) {
  if (conjunct->type() != Item::FUNC_ITEM) return;
  const Item_func *cnj = static_cast<const Item_func *>(conjunct);
  if (cnj->functype() == Item_func::EQ_FUNC) {
    Item *left_item = cnj->arguments()[0];
    Item *right_item = cnj->arguments()[1];
    /* (a,b)=(c,d) has been transformed to 'a=c and b=d' in
    Linear_comp_creator::create(), so this will be handled "automatically" as
    two conjuncts. (a,b)=(subquery) has not been transformed, and will be
    ignored. */
    assert(!(left_item->type() == Item::ROW_ITEM &&
             right_item->type() == Item::ROW_ITEM));
    analyze_scalar_eq(down_cast<const Item_func_eq *>(cnj), left_item,
                      right_item);
  }
}

bool Unique_analyzer::all_weak_side_is_fd() {
  /* Inspired by Group_check::is_fd_on_source(). 3 for() loops have been
  rewritten to STL algorithms. The goal is to compute the set of all tables of
  the weak side which are functionally dependent. To do this, we scan the
  functionally-dependent columns ('fd'), access their tables, and then study
  each such table. */
  for (auto item : fd) {
    Table_ref *tl =
        down_cast<const Item_field *>(item)->field->table->pos_in_table_list;
    table_map map = tl->map();
    if (tested_map_for_keys & map) continue;  // avoid repeating search
    tested_map_for_keys |= map;
    /* See if this weak-side tables is functionally dependent. _One_
    functionally dependent complete UNIQUE key is enough. */
    if (std::any_of(tl->table->key_info,
                    tl->table->key_info + tl->table->s->keys,
                    [this](auto &key_info) {
                      /* Only UNIQUE keys (including PRIMARY) are of interest.
                      It is ok if the key is on a prefix (so we do not ban
                      HA_PART_KEY_SEG): an equality imposes the value of the
                      full column, so of the prefix too, and there is at most
                      one row with such prefix. */
                      if (!(key_info.flags & HA_NOSAME)) return false;
                      /* For one key,  _all_ of its columns must be
                      functionally dependent. */
                      return std::all_of(
                          key_info.key_part,
                          key_info.key_part + key_info.user_defined_key_parts,
                          [this](auto &key_part) {
                            auto key_field = key_part.field;
                            /* A column is functionally dependent if it is
                            present at least _once_ in 'fd'. */
                            return std::any_of(
                                fd.begin(), fd.end(), [key_field](auto item2) {
                                  return static_cast<const Item_field *>(item2)
                                             ->field == key_field;
                                });
                          });
                    }))
      whole_tables_fd |= map;
    else if (tl->is_view_or_derived()) {
      /* Spot some pseudo-unique key, if the derived table contains GROUP BY
       or DISTINCT. As 'tl' was found from a TABLE object, it is materialized
       for sure. */
      assert(tl->uses_materialization());
      auto underlying_query_expression = tl->derived_query_expression();
      // UNION, EXCEPT, INTERSECT complicate logic => not considered.
      if (underlying_query_expression->is_simple()) {
        auto underlying_query_block =
            underlying_query_expression->first_query_block();
        if (underlying_query_block->is_distinct()) {
          /* This is similar to the code about unique keys above, except that
          'key_info' is replaced with a "single key" made of all selected
          expressions. */
          bool distinct_expr_found = true;
          auto select_list_size = underlying_query_block->num_visible_fields();
          for (size_t select_idx = 0; select_idx < select_list_size;
               ++select_idx) {
            // Is one column of 'fd' equal to this selected expression?
            distinct_expr_found =
                std::any_of(fd.begin(), fd.end(), [tl, select_idx](auto item2) {
                  auto item2_field =
                      static_cast<const Item_field *>(item2)->field;
                  return
                      /* If this column of 'fd' is a column of our derived table
                      and if it corresponds to the desired selected expression,
                      then such expression is determined. */
                      item2_field->table == tl->table &&
                      item2_field->field_index() == select_idx;
                });
            if (!distinct_expr_found) break;
          }
          if (distinct_expr_found) whole_tables_fd |= map;
        } else if (underlying_query_block->is_grouped()) {
          /* This is similar to the code about unique keys above, except that
          'key_info' is replaced with a "single key" made of all group
          expressions. */
          bool group_expr_found = true;
          for (ORDER *group = underlying_query_block->group_list.first; group;
               group = group->next) {
            Item *group_expr = *group->item;
            // Is one column of 'fd' equal to this group expression?
            group_expr_found = std::any_of(
                fd.begin(), fd.end(),
                [tl, underlying_query_block, group_expr](auto item2) {
                  auto item2_field =
                      static_cast<const Item_field *>(item2)->field;
                  if (item2_field->table == tl->table) {
                    // We have found in 'fd' a column of our derived table
                    uint idx = item2_field->field_index();
                    Item *select_expr_in_column = nullptr;
                    for (Item *item3 :
                         underlying_query_block->visible_fields()) {
                      if (idx == 0) {
                        select_expr_in_column = item3;
                        break;
                      } else {
                        --idx;
                      }
                    }
                    assert(select_expr_in_column != nullptr);
                    /* ROLLUP is ok; all it can do is add NULLs, which
                    will not match equalities anyway. */
                    select_expr_in_column =
                        unwrap_rollup_group(select_expr_in_column);
                    /* 'select_expr_in_column' is the expression which defines
                    the column item_field2 ; now see if it's equal to the
                    group expression ; if yes, we can say that the group
                    expression is determined. */
                    if (group_expr->eq(select_expr_in_column, false))
                      return true;
                  }
                  return false;
                });
            if (!group_expr_found) break;
          }
          if (group_expr_found) whole_tables_fd |= map;
        }
      }
    }
  }

  return whole_tables_fd == weak_tables;
}

}  // namespace

/**
   Does LEFT JOIN elimination in this Query_block.

   @param thd Thread handler
*/
void Query_block::do_join_elimination(THD *thd) {
  assert(first_execution);

  /* This query block may be just a part of a bigger statement, in which case
  elimination needs some care. For example, INSERT SELECT ON DUPLICATE KEY
  UPDATE: the UPDATE clause may reference tables from the SELECT part;
  these cannot be eliminated. */

  if (master_query_expression() != thd->lex->unit) {
    /* The query block is a subquery. Checking references only inside the
    subquery's query block will be enough: if anything external to the
    subquery does reference the subquery's tables, it may only be through the
    subquery's SELECT list, which is checked because it's part of the
    subquery's query block.
    Examples:
    INSERT INTO ... VALUES((subquery)) ,
    SELECT ... FROM (subquery) AS derived;
 */
  } else {
    // The query block is "top-level"
    switch (thd->lex->sql_command) {
      case SQLCOM_SELECT:
        // SELECT ...
      case SQLCOM_CREATE_TABLE:
        // As we have a query block, it is CREATE TABLE ... SELECT ...
      case SQLCOM_INSERT_SELECT:
        // INSERT INTO ... SELECT... We'll handle ON DUPLICATE KEY UPDATE.
      case SQLCOM_REPLACE_SELECT:
        // REPLACE INTO ... SELECT ... Cannot have ON DUPLICATE KEY UPDATE.
        break;
      default:
        return;
    }
  }

  // See IMPLEMENTATION_DEFAULT in the Doxygen comment at file's top.
  if (active_options() & OPTION_DEFAULT_WEAK_COLUMN) return;

  assert(outer_join != 0);
  assert(m_eliminated_tables == 0);

  /* There will be multiple recursive calls to do_join_elimination_for_list()
  possibly. Each of them will be on a different join nest, and want to compute
  the map of tables referenced from outside of this nest. However, a subset of
  this map is common to all calls: it is the map of tables referenced from
  outside of the FROM clause (f.ex. from WHERE, SELECT list...). For
  efficiency we will compute that subset only once and reuse it. It will be
  stored in this variable: */
  table_map tables_used_out_of_FROM{0};
  auto old_leaf_table_count = leaf_table_count;
  do_join_elimination_for_list(thd, &m_table_nest, &tables_used_out_of_FROM);
  if (leaf_table_count < old_leaf_table_count) {
    Opt_trace_context *trace = &thd->opt_trace;
    if (unlikely(trace->is_started())) {
      Opt_trace_object trace_wrapper(trace);
      Opt_trace_object trace_object(trace, "join_elimination");
      trace_object.add("number_of_eliminated_tables",
                       old_leaf_table_count - leaf_table_count);
      // the newly transformed query is worth printing
      opt_trace_print_expanded_query(thd, this, &trace_object);
    }
  }
}

static const auto walk_options =
    enum_walk::PREFIX | enum_walk::POSTFIX | enum_walk::SUBQUERY;

/**
   Does LEFT JOIN elimination on 'tables': for each element of 'tables',
   eliminate it if possible.

   @param thd Thread handler
   @param tables List of candidates to eliminate
   @param[in,out] tables_used_out_of_FROM Pointer to a table_map (details are
   in a comment in do_join_elimination()).
*/
void Query_block::do_join_elimination_for_list(
    THD *thd, mem_root_deque<Table_ref *> *tables,
    table_map *tables_used_out_of_FROM) {
  for (auto li = tables->begin(); li != tables->end();) {
    Table_ref *const table = *li;
    table_map weak_tables{0};
    if (table->nested_join != nullptr) {
      // join nest containing >1 tables; do elimination inside the nest first
      do_join_elimination_for_list(thd, &table->nested_join->m_tables,
                                   tables_used_out_of_FROM);
      weak_tables = table->nested_join->used_tables;
    } else
      weak_tables = table->map();

    auto cond = table->join_cond();

    if (cond == nullptr ||
        table->is_sj_or_aj_nest()) {  // not the right argument of a left join
      ++li;
      continue;
    }

    /* 'table' is to the right of a left join. Now we check if it's not
    referenced from anywhere else than the ON clause. */

    if (*tables_used_out_of_FROM == 0) {
      /* No call to this function has computed this common map yet. So let's
      do it now and only once. */
      table_map map{0};

      /* We need to walk all clauses of the Query_block except
      m_current_table_nest. Why not use something as simple as

      WalkQueryBlock(this, [&](Item *item) {
        map |= item->used_tables();
        return false;
      });

      ? Because it has a fundamental inefficiency for us here: walk() will
      explore each item, by design. F. ex. if we have a+b, it will explore '+'
      then 'a' and 'b'. But used_tables() of '+' is all we need, it already
      contains used_tables() of 'a' and 'b'. We could use the "stop_at"
      technique of Item_tree_walker, but that would still explore 'a' and 'b',
      just not call used_tables() on them : we would still have the full tree
      traversal. We could use compile() which allows skipping parts of an Item
      tree, but there is no Query_block::compile(), only Item::compile(). So
      we just collect used_tables() of each member of SELECT, WHERE, GROUP
      BY...

      Now about the use of used_tables(). There is unfortunately a catch when
      the item is Item_sum; in Item_sum::add_used_tables_for_aggr_func() we
      see that for an aggregate or window function used_tables() is the map of
      all tables of the FROM clause (and then this "too big map" propagates to
      any item which contains the Item_sum). So for a query like:

      SELECT 2+COUNT(table1.col*3) FROM table1 LEFT JOIN table2
      ON table1.col=table2.unique_col;

      the value of used_tables() for '+' makes our code think that the
      weak-side table is referenced by '+', which is wrong and defeats join
      elimination. Therefore, we have special code for when an item contains
      Item_sum: we use Item::compile() to calculate the "true" value of
      used_tables() : which tables it really uses, i.e. what its arguments
      use. */
      auto ut = [](Item *item) -> table_map {
        table_map ut_map{0};
        CompileItem(
            item,
            // The logic is in the analyzer.
            [&ut_map](Item *inner_item) {
              if (inner_item->has_wf() || inner_item->has_aggregation()) {
                /* used_tables() is unreliable => slow path. Add no bits and let
                the arguments add their bits. This is the case of '+' in
                the query above. We add no bits for '+', go down to '2' and
                add bits for it (none actually as it's a literal), go to
                COUNT and add no bits, go down to '*' and add bits for it
                and do not go down to table1.col.

                We have a special case: if the window function or aggregate is
                itself in a subquery of the current Query_block, we will not
                be able to reach to its arguments, because
                Item_subselect::compile() does not dive into its underlying
                Query_block's items; moreover,
                Item_subselect::update_used_tables(), which we would need to
                call further down after actual elimination, doesn't dive
                either. We declare that elimination is not possible in this
                case, by pretending that all tables are referenced. */
                if (inner_item->type() == Item::SUBSELECT_ITEM) {
                  ut_map = ~table_map{0};
                  return false;
                }
                return true;  // Reach to arguments.
              } else {
                // used_tables() is reliable => fast path.
                ut_map |= inner_item->used_tables();
                return false;  // Do not reach to arguments, no need.
              }
            },
            // Dummy transformer.
            [](Item *inner_item) { return inner_item; });
        return ut_map;
      };
      /* To make it clear: if an item of the clauses below contains no
      Item_sum, compile() will just get its used_tables() and not go down into
      its arguments ; in this case we have no Item traversal and only one call
      to used_tables(), so should be reasonably efficient. */
      for (Item *item : visible_fields()) map |= ut(item);
      if (where_cond() != nullptr) map |= ut(where_cond());
      if (having_cond() != nullptr) map |= ut(having_cond());
      for (ORDER *group = group_list.first; group != nullptr;
           group = group->next)
        map |= ut(*group->item);
      for (ORDER *order = order_list.first; order != nullptr;
           order = order->next)
        map |= ut(*order->item);
      List_iterator<Window> liw(m_windows);
      for (Window *w = liw++; w != nullptr; w = liw++)
        for (auto it : {w->first_partition_by(), w->first_order_by()})
          if (it != nullptr)
            for (ORDER *o = it; o != nullptr; o = o->next) map |= ut(*o->item);

      if (thd->lex->sql_command == SQLCOM_INSERT_SELECT) {
        // Add references found in ON DUPLICATE KEY UPDATE x=y (only 'y').
        for (auto item : down_cast<Sql_cmd_insert_base *>(thd->lex->m_sql_cmd)
                             ->update_value_list)
          map |= ut(item);
      }

      *tables_used_out_of_FROM = map;
    }

    if (*tables_used_out_of_FROM & weak_tables) {
      // one quick check, which could save the upcoming walk_join_list()
      ++li;
      continue;
    }

    /* Now, compute the part of map which is specific of this elimination
    round : map of tables referenced in the FROM, but not in the current nest.
    In other words:

    for: t1 LEFT JOIN (t2 LEFT JOIN t3 ON cond23) ON cond123 ,

    if we are considering eliminating t3, we want to collect table maps used
    by cond123, not by cond23; if cond123 does not reference t3 we can
    eliminate t3, and it's irrelevant if cond23 references t3 (likely it does,
    btw). */
    table_map tables_used_in_FROM_out_of_cur_nest{0};

    /* The visit must skip the current nest entirely. This requires two
    things.

    1. When the walk is on our table, we make it do nothing.

    2. But the walk is still going down into our table if it's a nest. So we
    make it look like one single table, preventing any dive into underlying
    tables. This matters for:

     t1 LEFT JOIN (t2 LEFT JOIN t3 ON cond23) ON cond123

    when we consider eliminating the nest t2-t3: it's irrelevant if cond23
    references t2 and t3 (and it's also irrelevant if cond123 references them,
    but that is already taken care of by point 1).

    Finally, note that below we do not have the problem of "Item_sum's
    used_tables too big" previously described, as in the ON condition (like in
    WHERE) there cannot be Item_sum. */
    auto saved_nested_join = table->nested_join;
    table->nested_join = nullptr;
    walk_join_list(
        m_table_nest,
        [&tables_used_in_FROM_out_of_cur_nest, table](Table_ref *tr) -> bool {
          if (tr == table) return false;
          if (tr->join_cond())
            tables_used_in_FROM_out_of_cur_nest |=
                tr->join_cond()->used_tables();
          if (tr->is_derived() && tr->uses_materialization())
            /* A merged derived table is a join nest at this point, we must
            not access its derived_query_expression() (which is 0x1 btw). */
            tables_used_in_FROM_out_of_cur_nest |=
                tr->derived_query_expression()->m_lateral_deps;
          else if (tr->is_table_function())
            tables_used_in_FROM_out_of_cur_nest |=
                tr->table_function->used_tables();
          return false;
        });
    table->nested_join = saved_nested_join;

    if (tables_used_in_FROM_out_of_cur_nest & weak_tables) {
      ++li;
      continue;
    }

    bool can_eliminate = false;
    if (is_grouped() || is_distinct()) {
      /* SELECT T1.A, COUNT(*) FROM T1 LEFT JOIN T2 ON cond GROUP BY T1.A;
      cannot do LJE because the presence of T2 introduces several rows in
      the group of a value of T1.A which changes the value of COUNT on this
      group, compared to if T2 were absent.
      Same for
      SELECT DISTINCT COUNT(*) FROM T1 LEFT JOIN T2 ON cond;
      In other words, an aggregate is a problem because it depends on
      cardinality _and_ it is computed _before_ de-duplication (GROUP BY,
      DISTINCT).

      SELECT T1.A, ROW_NUMBER(*) OVER() FROM T1 LEFT JOIN T2 ON cond
        GROUP BY T1.A;
      can do LJE, because window functions are calculated after
      de-duplication (GROUP BY).
      SELECT DISTINCT T1.A, ROW_NUMBER(*) OVER() FROM T1 LEFT
        JOIN T2 ON cond;
      cannot do LJE, because window functions are calculated before
      de-duplication (DISTINCT).

      LIMIT is also cardinality-dependent; however it is applied after
      de-duplication (GROUP BY or DISTINCT), so is acceptable. */
      if (!agg_func_used() && (!has_windows() || is_grouped()))
        can_eliminate = true;
    }
    if (!can_eliminate) {
      // See if the left join is in the right side of a semi-join or anti-join
      for (auto tl = table->embedding; tl != nullptr; tl = tl->embedding) {
        if (tl->is_sj_or_aj_nest()) {
          can_eliminate = true;
          break;
        }
      }
    }
    if (!can_eliminate) {
      /* See if the left join is in a subquery of type IN or EXISTS (not
      transformed to semi-join or anti-join for some reason), containing no
      item depending on cardinality. */
      auto owning_subquery_item = master_query_expression()->item;
      if (owning_subquery_item != nullptr) {
        switch (owning_subquery_item->substype()) {
          case Item_exists_subselect::IN_SUBS:
            assert(select_limit == nullptr);
            [[fallthrough]];
          case Item_exists_subselect::EXISTS_SUBS:
            can_eliminate = !agg_func_used() && !has_windows();
            /* LIMIT is also cardinality-dependent. For EXISTS, it is
            irrelevant (one row is the same as many rows). For IN, LIMIT will
            keep only certain values, which influences what IN will compare
            to, and it is applied before de-duplication (which is IN check
            over the subquery's complete result), so it must be excluded.
            Fortunately, LIMIT in IN is not supported by MySQL. */
            [[fallthrough]];
          default:
            break;
        }
      }
    }
    if (!can_eliminate) {
      /* Now search for a unique key of the weak side, which would be entirely
      determined by the ON condition. For this, the interesting comparison
      operators in the ON condition are: equality. Greater-than and the like
      are obviously not. And <=> is not because there can be two NULLs in a
      UNIQUE column : thus if we have one NULL on the strong side, and two
      NULLs on the weak side, with <=> we'll have two rows in the join's
      result.

      If we find enough interesting equality conditions to guarantee that the
      LEFT JOIN has at most one match in the weak side, then there may be
      other non-interesting conditions, linked all together with AND; they do
      not change the fact that there is at most one match. */

      Unique_analyzer unique_analyzer(this, weak_tables, thd->mem_root);
      if (cond->type() == Item::COND_ITEM) {
        Item_cond *cnd = static_cast<Item_cond *>(cond);
        /* All ANDs already flattened, see:
        "(X1 AND X2) AND (Y1 AND Y2) ==> AND (X1, X2, Y1, Y2)"
        in sql_yacc, and also Item_cond::fix_fields(). */
        if (cnd->functype() != Item_func::COND_AND_FUNC) return;
        List_iterator<Item> li_args(*(cnd->argument_list()));
        Item *item;
        while ((item = li_args++)) unique_analyzer.analyze_conjunct(item);
      } else  // only one conjunct
        unique_analyzer.analyze_conjunct(cond);
      can_eliminate = unique_analyzer.all_weak_side_is_fd();
    }

    /* If the ON condition is constant and always FALSE, we could also
    eliminate the join, but that sounds like an unrealistic situation, and
    the Optimizer already checks that and then marks the weak table as
    constant, which is good enough (this works only for a single table on
    the weak side though). */

    if (can_eliminate) {
      /* Scan leaf_tables and remove from it the tables which have a bit in
      weak_tables. Correct leaf_table_count. */
      bool removed_a_table [[maybe_unused]] = false;
      Table_ref *prev_tl = nullptr;
      for (auto tl = leaf_tables; tl; tl = tl->next_leaf) {
        if (tl->map() & weak_tables) {
          if (prev_tl)
            prev_tl->next_leaf = tl->next_leaf;
          else
            leaf_tables = tl->next_leaf;
          assert(leaf_table_count >= 2);  // A LEFT JOIN has 2 arguments
          leaf_table_count--;
          if (tl->table != nullptr) {
            /* This is to detect bugs: if it turns out that some reference to
            this table remained somewhere, when a read of any column is tried
            from that reference it should trigger a failure of the assertion
            which checks that the column is marked in read_set. */
            bitmap_clear_all(tl->table->read_set);
          }
          removed_a_table = true;
        } else
          prev_tl = tl;
      }
      assert(removed_a_table);
      // Fix bitmaps of parent join nests
      for (auto tl = table->embedding; tl != nullptr; tl = tl->embedding) {
        tl->nested_join->used_tables &= ~weak_tables;
        tl->nested_join->not_null_tables &= ~weak_tables;
        tl->nested_join->sj_corr_tables &= ~weak_tables;
        tl->nested_join->sj_depends_on &= ~weak_tables;
        tl->dep_tables &= ~weak_tables;
        tl->join_cond_dep_tables &= ~weak_tables;
        tl->sj_inner_tables &= ~weak_tables;
      }
      // Correct the work done by record_join_nest_info().
      outer_join &= ~weak_tables;
      for (auto sj_list_it = sj_nests.begin(); sj_list_it != sj_nests.end();) {
        auto sj_nest = *sj_list_it;
        // If the semi-join nest is included in what we are removing:
        if ((sj_nest->sj_inner_tables & ~weak_tables) == 0)
          sj_list_it = sj_nests.erase(sj_list_it);
        else
          ++sj_list_it;
      }

      if (li != tables->begin()) {
        /* Remember that the 'tables' list is in reverse order of FROM. So we
        come here if our current table is not the last table in the
        order of FROM.

        Consider
        SELECT DISTINCT t3.*
          FROM t1 LEFT JOIN t2 ON ... LEFT JOIN t3 ON COND;
        and COND does not depend on t2 so we are eliminating t2.

        However, an artificial dependency "t3 depends on t2" may be declared
        by simplify_joins() (look for "If join condition contains no reference
        to outer tables" in such function), for example if COND is just a
        constant TRUE ; the goal being to ensure that t3 is read after t2.
        This dependency, if we leave it in place, can have bad consequences:
        the optimizer may think that t3 can never be read, because it's
        waiting for t2 to be read (according to the dependency), but t2 will
        not be read as it's eliminated; or if the optimizer rather ignores
        this dependency (because t2 is not part of JOIN_TABs), it may put t3
        before t1, preventing correct execution of "t1 LEFT JOIN t3". So we
        need to replace in t3->dep_tables the map of t2 with the map of t1.
        More generally: replace the map of the removed table with the map of
        the table right before the removed one.

        prev_table is the next table in the order of FROM. */
        Table_ref *prev_table = *(li - 1);
        if (prev_table->dep_tables & weak_tables) {
          /* Yes there is a problematic reference, so replace the bits. Cannot
          be first in the order of FROM, as the removed table is on the right
          side of a LEFT JOIN. */
          assert(li != tables->end());
          Table_ref *next_table = *(li + 1);
          prev_table->dep_tables &= ~weak_tables;
          prev_table->dep_tables |= (next_table->nested_join != nullptr)
                                        ? next_table->nested_join->used_tables
                                        : next_table->map();
        }
      }

      /* Imagine we have
      t1 LEFT JOIN t2 ON cond_with_subq,
      and we are currently removing t2. We clean up cond_with_subq, which
      detaches the subquery's query expression from the tree of query
      expressions so that we do not waste time in doing JOIN::optimize() on
      it, and do not show rows for it in EXPLAIN FORMAT=TRADITIONAL (this is
      not a "must", rather a "nice to have").

      Also imagine that t2 is actually a materialized derived table: we would
      like to detach its query expression, again to not optimize it. However,
      note that we detach it from the chain of query expressions, but not from
      the table itself: this table must remain a derived table, so that
      cleanup_tmp_tables() properly spots it and destroys the associated
      temporary table. So we do not alter the pointers in
      derived_query_expression() and common_table_expr(). */

      auto table_cleaner = [query_block = this](Table_ref *tr) -> bool {
        if (tr->join_cond()) {
          Item::Cleanup_after_removal_context ctx(query_block);
          tr->join_cond()->walk(&Item::clean_up_after_removal, walk_options,
                                pointer_cast<uchar *>(&ctx));
        }
        if (tr->is_view_or_derived() && tr->uses_materialization()) {
          auto cte = tr->common_table_expr();
          if (cte != nullptr &&
              (cte->references.size() >= 2 || cte->recursive)) {
            /* This CTE is referenced from other places than here, so it is
            too dangerous to detach its query expression (maybe it will serve
            for shared materialization of the CTE?). As a consequence of not
            detaching, the expression will run through JOIN::optimize() and
            show up in EXPLAIN FORMAT=TRADITIONAL; which may very well be
            needed if other references are not eliminated; in the extreme case
            where they all are eliminated, it's not optimal but we have to
            accept this price. */
          } else {
            // This un-plugs the body of the derived table
            tr->derived_query_expression()->exclude_tree();
            if (cte != nullptr) cte->references.clear();
          }
        } else if (tr->is_table_function()) {
          /* Probably this is not needed as JSON_TABLE may
           cannot contain an aggregate or a subquery, but from
           a logical POV it makes sense. */
          Item::Cleanup_after_removal_context ctx(query_block);
          tr->table_function->walk(&Item::clean_up_after_removal, walk_options,
                                   pointer_cast<uchar *>(&ctx));
          /* At this stage, setup_table_function() and
          setup_materialized_derived() have been called, therefore table
          functions and materialized derived tables have a TABLE (tr->table).
          This TABLE must be destroyed. It is simpler to leave this task to
          the usual code which does that at the end of execution
          (cleanup_tmp_tables()), rather than doing something specific here.
          To this aim, all we have to do is to allow the table to stay in
          m_table_list. That does sound inconsistent, but code inspection of
          the usage of m_table_list suggests that it's actually safe. As a
          bonus, the usual code will also call cleanup() on the table
          function. */
        }
        return false;
      };

      table_cleaner(table);
      if (table->nested_join != nullptr) {
        /* Imagine we have
        t1 LEFT JOIN (t2 JOIN t3 ON cond_with_subq) ON cond,
        and we are currently removing the nest t2-t3. We clean up
        cond_with_subq. Also imagine that t3 is actually a materialized
        derived table: we would like to detach its query expression. */
        walk_join_list(table->nested_join->m_tables, table_cleaner);
      }
      m_eliminated_tables |= weak_tables;
      // remove 'table' from the current list, and destroy
      li = tables->erase(li);
      // 'li' already points to the next table so we don't advance it
      continue;
    }
    ++li;
  }
  if ((m_eliminated_tables != 0) && (agg_func_used() || has_windows())) {
    /* As mentioned earlier above, aggregates and window functions have 'all
      tables' in their used_tables(), even the eliminated ones. We need to
      force an update on them; otherwise this will confuse further logic in
      the optimizer. */
    auto ut = [](Item *item) -> table_map {
      table_map ut_map{0};
      CompileItem(
          item,
          // The logic is in the analyzer.
          [&ut_map](Item *inner_item) {
            if (inner_item->has_wf() || inner_item->has_aggregation())
              inner_item->update_used_tables();
            ut_map |= inner_item->used_tables();
            /* Why we do not need to reach to arguments. First,
               update_used_tables() is itself recursive. Second, it has
               updated the Item_sum's used_tables() so that the eliminated
               tables are not listed in the map anymore. Which is all we need
               to ensure - ut_map is only used in the final assertion a bit
               further. */
            return false;
          },
          // Dummy transformer.
          [](Item *inner_item) { return inner_item; });
      return ut_map;
    };
    table_map map = 0;
    /* Aggregates and window functions which reference eliminated tables can
       exist only in certain clauses (e.g. not in WHERE). */
    for (Item *item : visible_fields()) map |= ut(item);
    if (having_cond() != nullptr) map |= ut(having_cond());
    for (ORDER *order = order_list.first; order != nullptr; order = order->next)
      map |= ut(*order->item);
    List_iterator<Window> liw(m_windows);
    for (Window *w = liw++; w != nullptr; w = liw++)
      for (auto it : {w->first_partition_by(), w->first_order_by()})
        if (it != nullptr)
          for (ORDER *o = it; o != nullptr; o = o->next) map |= ut(*o->item);
    // If the recalculation of used_tables went fine, eliminated tables should
    // not be referenced anymore:
    assert((map & m_eliminated_tables) == 0);
  }
}
