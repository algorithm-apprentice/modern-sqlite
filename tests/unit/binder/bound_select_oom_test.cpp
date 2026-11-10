#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "modern_sqlite/base/bytes.hpp"
#include "modern_sqlite/binder/bound_statement.hpp"
#include "modern_sqlite/catalog/catalog.hpp"
#include "modern_sqlite/runtime/sql_value.hpp"
#include "modern_sqlite/syntax/parser.hpp"
#include "tests/support/aligned_allocation.hpp"

namespace {

std::atomic<std::size_t> allocation_index = 0;
std::size_t failing_allocation = 0;
bool inject_failure = false;

[[nodiscard]] void* Allocate(std::size_t size) {
  const std::size_t index = allocation_index.fetch_add(1, std::memory_order_relaxed);
  if (inject_failure && index == failing_allocation) {
    throw std::bad_alloc{};
  }
  if (void* memory = std::malloc(size == 0 ? 1 : size); memory != nullptr) {
    return memory;
  }
  throw std::bad_alloc{};
}

[[nodiscard]] void* AllocateAligned(std::size_t size, std::size_t alignment) {
  const std::size_t index = allocation_index.fetch_add(1, std::memory_order_relaxed);
  if (inject_failure && index == failing_allocation) {
    throw std::bad_alloc{};
  }
  void* memory = nullptr;
  if (modern_sqlite::test_support::AllocateAligned(&memory, alignment,
                                                   size == 0 ? alignment : size) == 0) {
    return memory;
  }
  throw std::bad_alloc{};
}

[[nodiscard]] modern_sqlite::SyntaxTree ParseTree(std::string_view sql) {
  modern_sqlite::ParseResult parsed = modern_sqlite::ParseOne(modern_sqlite::Utf8View{sql});
  if (!parsed.has_value() || !parsed->tree.has_value()) {
    throw std::runtime_error{"failed to parse binder OOM fixture"};
  }
  return std::move(*parsed->tree);
}

[[nodiscard]] modern_sqlite::CatalogSnapshotPtr TestCatalog() {
  using namespace modern_sqlite;

  CatalogInput input{
      .schema_name = "main",
      .version = CatalogVersion{.schema_cookie = 1, .generation = 1},
  };
  input.definitions.push_back(
      ParseTree("CREATE TABLE Items(id INTEGER PRIMARY KEY, Name TEXT COLLATE NOCASE)"));
  input.tables.push_back(CatalogTableInput{
      .definition = SchemaDefinitionId{0},
      .name = "Items",
      .root_page = RootPageId{2},
      .columns =
          {
              CatalogColumnInput{
                  .name = "id",
                  .declared_type = "INTEGER",
                  .primary_key = true,
              },
              CatalogColumnInput{
                  .name = "Name",
                  .declared_type = "TEXT",
                  .collation_name = "NOCASE",
              },
          },
      .rowid_alias = ColumnId{0},
  });
  std::string negations;
  for (std::size_t index = 0; index < 32U; ++index) {
    negations += "- ";
  }
  input.definitions.push_back(
      ParseTree("CREATE TABLE DefaultOwner("
                "a TEXT DEFAULT 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789',"
                "b BLOB DEFAULT X'00112233445566778899AABBCCDDEEFF',"
                "c TEXT DEFAULT(5.00),deep TEXT DEFAULT(" +
                negations + "1))"));
  input.definitions.push_back(
      ParseTree("CREATE TABLE InvalidDefaults(a TEXT DEFAULT(0x10000000000000000),"
                "b TEXT DEFAULT(0x20000000000000000))"));
  input.definitions.push_back(
      ParseTree("CREATE TABLE UnaryDefaults("
                "a BLOB DEFAULT(-'5.0abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ'),"
                "b BLOB DEFAULT(-X'352e30000102030405060708090a0b0c0d0e0f'),"
                "c BLOB DEFAULT(-NULL),deep BLOB DEFAULT(" +
                negations + "1.5))"));
  input.definitions.push_back(
      ParseTree("CREATE TABLE InvalidNegativeDefaults(a BLOB DEFAULT(-0x8000000000000000),"
                "b BLOB DEFAULT(0x1_0000_0000_0000_0000))"));
  constexpr std::array table_names{"DefaultOwner", "InvalidDefaults", "UnaryDefaults",
                                   "InvalidNegativeDefaults"};
  for (std::size_t definition = 1; definition < input.definitions.size(); ++definition) {
    const auto& syntax = std::get<CreateTableStatement>(input.definitions[definition].statement());
    CatalogTableInput table{
        .definition = SchemaDefinitionId{definition},
        .name = table_names[definition - 1U],
        .root_page = RootPageId{static_cast<std::uint32_t>(definition + 2U)},
    };
    constexpr std::array names{"a", "b", "c", "deep"};
    for (std::size_t column = 0; column < syntax.columns.size(); ++column) {
      const auto& constraint =
          std::get<DefaultColumnConstraint>(syntax.columns[column].constraints.front().payload);
      SqlValue extracted;
      if (definition == 4U) {
        extracted = column == 0U ? SqlValue::Real(9223372036854775808.0)
                                 : SqlValue::Text("0x10000000000000000");
      } else if (definition == 3U) {
        if (column < 2U) {
          extracted = SqlValue::Integer(-5);
        } else if (column == 3U) {
          extracted = SqlValue::Real(1.5);
        }
      } else if (definition == 2U) {
        extracted = SqlValue::Text(column == 0U ? "0x10000000000000000" : "0x20000000000000000");
      } else if (column == 0U) {
        extracted =
            SqlValue::Text("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");
      } else if (column == 1U) {
        ByteBuffer bytes{ByteCount{16}};
        for (std::size_t index = 0; index < 16U; ++index) {
          bytes.mutable_view()[index] = static_cast<std::byte>(index * 0x11U);
        }
        extracted = SqlValue::Blob(std::move(bytes));
      } else {
        extracted = SqlValue::Text(column == 2U ? "5.00" : "1");
      }
      table.columns.push_back(CatalogColumnInput{
          .name = names[column],
          .declared_type = definition >= 3U || (definition == 1U && column == 1U) ? "BLOB" : "TEXT",
          .default_expression = SchemaExpression{.definition = SchemaDefinitionId{definition},
                                                 .expression = constraint.expression},
          .missing_record_value = std::make_shared<const SqlValue>(std::move(extracted)),
      });
    }
    input.tables.push_back(std::move(table));
  }
  CatalogSnapshotResult created = CatalogSnapshot::Create(std::move(input));
  if (!created.has_value()) {
    throw std::runtime_error{"failed to create binder OOM catalog"};
  }
  return *std::move(created);
}

}  // namespace

void* operator new(std::size_t size) { return Allocate(size); }
void* operator new[](std::size_t size) { return Allocate(size); }
void* operator new(std::size_t size, std::align_val_t alignment) {
  return AllocateAligned(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
  return AllocateAligned(size, static_cast<std::size_t>(alignment));
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete(void* memory, std::align_val_t) noexcept {
  modern_sqlite::test_support::FreeAligned(memory);
}
void operator delete[](void* memory, std::align_val_t) noexcept {
  modern_sqlite::test_support::FreeAligned(memory);
}
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept {
  modern_sqlite::test_support::FreeAligned(memory);
}
void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept {
  modern_sqlite::test_support::FreeAligned(memory);
}

int main() try {
  using namespace modern_sqlite;

  constexpr std::string_view kAggregateSql =
      "SELECT id,group_concat(DISTINCT Name ORDER BY id) "
      "FILTER (WHERE id>0) FROM Items WHERE id>0 "
      "GROUP BY id HAVING id>0 ORDER BY sum(id)";
  constexpr std::array kSql{
      std::string_view{"SELECT Name COLLATE NOCASE, abs(?), coalesce(NULL,Name), "
                       "x'00112233445566778899AABBCCDDEEFF' AS payload "
                       "FROM Items WHERE Name=? AND id>0 "
                       "ORDER BY payload DESC NULLS FIRST,+id LIMIT ?"},
      std::string_view{"VALUES(?1,?2 IS NULL),(?3,?4) "
                       "UNION SELECT Name,id FROM Items "
                       "ORDER BY 1 COLLATE NOCASE LIMIT ?5"},
      kAggregateSql,
      std::string_view{"INSERT INTO Items(Name,id) VALUES(abs(?1),?2)"},
      std::string_view{"UPDATE Items SET Name=coalesce(?1,Name), id=id+1 WHERE Name=?2"},
      std::string_view{"DELETE FROM Items WHERE id=?1"},
      std::string_view{"INSERT INTO Items VALUES(?1,(SELECT Name FROM Items WHERE id=?2))"},
      std::string_view{"UPDATE Items SET Name=(SELECT (SELECT q.Name) FROM Items q "
                       "ORDER BY q.id DESC) WHERE EXISTS(SELECT ?1)"},
      std::string_view{"DELETE FROM Items WHERE Name=(SELECT min(Name) FROM Items)"},
      std::string_view{"CREATE TABLE NewItems(id INTEGER PRIMARY KEY, name TEXT DEFAULT 'x')"},
      std::string_view{"CREATE UNIQUE INDEX items_name ON Items(Name COLLATE NOCASE DESC)"},
      std::string_view{"ANALYZE Items"},
  };
  const CatalogSnapshotPtr catalog = TestCatalog();

  for (const std::string_view sql : kSql) {
    SyntaxTree baseline_tree = ParseTree(sql);
    allocation_index.store(0, std::memory_order_relaxed);
    const BindStatementResult baseline = BindStatement(std::move(baseline_tree), catalog);
    if (!baseline.has_value()) {
      return 1;
    }
    const std::size_t allocation_count = allocation_index.load(std::memory_order_relaxed);
    if (allocation_count == 0 || allocation_count > 1024U) {
      return 1;
    }

    const std::array<std::size_t, 5> failures{
        0,
        allocation_count / 4U,
        allocation_count / 2U,
        (allocation_count * 3U) / 4U,
        allocation_count - 1U,
    };
    std::optional<std::size_t> previous_failure;
    for (const std::size_t failure : failures) {
      if (previous_failure == failure) {
        continue;
      }
      previous_failure = failure;
      SyntaxTree tree = ParseTree(sql);
      allocation_index.store(0, std::memory_order_relaxed);
      failing_allocation = failure;
      inject_failure = true;
      bool threw = false;
      try {
        [[maybe_unused]] const BindStatementResult unexpected =
            BindStatement(std::move(tree), catalog);
      } catch (const std::bad_alloc&) {
        threw = true;
      }
      inject_failure = false;
      if (!threw) {
        return 1;
      }
    }

    const BindStatementResult recovered = BindStatement(ParseTree(sql), catalog);
    if (!recovered.has_value()) {
      return 1;
    }
  }

  const std::array<std::string_view, 89> full_sweep_sqls{
      kAggregateSql,
      "SELECT 7.0 AS a,2 AS b UNION ALL SELECT 3,7.00 ORDER BY 7.00",
      "SELECT sum((SELECT 1)) AS x FROM Items "
      "HAVING sum((SELECT 2))>0 ORDER BY x",
      "SELECT sum((SELECT Name)) AS x FROM Items "
      "UNION ALL SELECT sum((SELECT id)) FROM Items ORDER BY x",
      "SELECT (SELECT sum(o.id)) FROM Items o",
      "SELECT (VALUES(8),(sum((SELECT o.id+?1)))) AS v FROM Items o ORDER BY v",
      "SELECT sum((SELECT 1)) AS x FROM Items "
      "HAVING (SELECT x)>0 ORDER BY (SELECT x)",
      "SELECT EXISTS(SELECT 1 ORDER BY sum(o.id)) FROM Items o",
      "SELECT ("
      "SELECT sum((SELECT 1)+0*o.id)+sum((SELECT 1)+0*i.id) AS x "
      "FROM Items i UNION ALL SELECT 0 ORDER BY x"
      ") FROM Items o",
      "SELECT count(*) AS x FROM Items o HAVING ("
      "SELECT sum((SELECT x UNION ALL "
      "SELECT sum(i.id) FROM Items i ORDER BY sum(id) LIMIT 1))"
      ")>0",
      "SELECT a.*,b.Name FROM Items a CROSS JOIN Items b WHERE a.id=?1 "
      "ORDER BY b.Name LIMIT ?2",
      "SELECT a.Name FROM Items a,Items b UNION SELECT ?1 ORDER BY 1",
      "SELECT b.id,a._rowid_,max(a.id),group_concat(a.Name ORDER BY b.id) "
      "FILTER (WHERE a.id>?1) FROM Items a CROSS JOIN Items b GROUP BY b.id ORDER BY 1",
      "SELECT a.id AS key,b.Name,count(b.id) FROM Items a LEFT JOIN Items b "
      "ON key=b.id+?1 GROUP BY a.id ORDER BY 1",
      "SELECT *,b.* FROM Items a LEFT JOIN Items b USING(id)",
      "SELECT id,count(b.id) FROM Items a NATURAL LEFT JOIN Items b "
      "GROUP BY id ORDER BY id",
      "SELECT *,b.* FROM Items a RIGHT JOIN Items b USING(id)",
      "SELECT id,count(a.id) FROM Items a NATURAL RIGHT JOIN Items b GROUP BY id ORDER BY 1",
      "SELECT *,b.* FROM Items a FULL JOIN Items b USING(id)",
      "SELECT q.* FROM Items q NATURAL FULL JOIN Items q",
      "SELECT (SELECT (SELECT o.Name)),EXISTS(SELECT 1 ORDER BY (SELECT o.Name)),"
      "(VALUES(o._rowid_)) FROM Items o",
      "SELECT count(*),EXISTS(SELECT o.Name ORDER BY o.id) FROM Items o",
      "SELECT (SELECT group_concat(i.Name ORDER BY i.id) FILTER (WHERE i.id>=o.id) "
      "FROM Items i GROUP BY i.Name) FROM Items o",
      "SELECT o.id,(SELECT DISTINCT i.Name FROM Items i WHERE i.id<=o.id "
      "ORDER BY i.Name LIMIT ?1) AS x FROM Items o WHERE x IS NOT NULL",
      "SELECT (SELECT id) FROM Items a FULL JOIN Items b USING(id)",
      "SELECT 1 AS x FROM Items WHERE (SELECT (SELECT x))>0",
      "SELECT Name AS x FROM Items WHERE EXISTS(SELECT x ORDER BY (SELECT x) LIMIT 1)",
      "SELECT Name AS x,count(*) FROM Items GROUP BY Name HAVING (SELECT x) IS NOT NULL",
      "SELECT a.*,a.id,b.id FROM Items a RIGHT JOIN Items b USING(id)",
      "SELECT a.*,a.id,b.id FROM Items a FULL JOIN Items b USING(id)",
      "SELECT b.*,a.id,b.id,c.id FROM Items a LEFT JOIN Items b USING(id) "
      "RIGHT JOIN Items c USING(id)",
      "SELECT a.*,b.*,c.id FROM Items a FULL JOIN Items b USING(id) "
      "FULL JOIN Items c USING(id)",
      "SELECT EXISTS(SELECT a.* FROM Items a FULL JOIN Items b USING(id)) FROM Items o",
      "SELECT (VALUES(1),(2),(id),(4),(5)) FROM Items",
      "SELECT (VALUES(?1),(id+0x10000000000000000)) AS v FROM Items WHERE v",
      "UPDATE Items SET Name=(VALUES(abs(?1)),(?2)) WHERE id=?3",
      "SELECT (VALUES(1),(ifnull(1))) IN ()",
      "SELECT (VALUES(1),(likelihood(?1,2)) UNION ALL SELECT 9 LIMIT 1 OFFSET 1)",
      "VALUES(0),(likelihood('a','ignored' COLLATE NOCASE)='A')",
      "VALUES(0),(abs(-9223372036854775808) OR likelihood(0 OR 1,99))",
      "SELECT iif(likely(NULL AND abs(-9223372036854775808)),7,8)",
      "SELECT EXISTS(SELECT 0x10000000000000000,1_2.5e1,'it''s',x'CAFE',?9 FROM Items)",
      "SELECT EXISTS(SELECT 'it''s a retained literal' AS x FROM Items GROUP BY x,x)",
      "SELECT EXISTS(SELECT (SELECT 0x10000000000000000) AS x FROM Items ORDER BY x)",
      "UPDATE Items SET Name=-0x10000000000000000,Name=EXISTS(SELECT 0x10000000000000000)",
      "SELECT missing_name AND 0,0x10000000000000000 IS NULL,0 AND ?9",
      "SELECT 0 AND (SELECT missing_function(1) UNION ALL SELECT ?7)",
      "INSERT INTO Items(id,Name) VALUES(1,missing_name AND 0)",
      "UPDATE Items SET Name=missing_name AND 0 WHERE id=?9",
      "SELECT (SELECT 1) AS q FROM Items WHERE +q OR id",
      "SELECT (SELECT id FROM Items a FULL JOIN Items b USING(id) LIMIT 1)",
      "SELECT sum(id ORDER BY (SELECT 1)) FILTER (WHERE (SELECT 1)) FROM Items",
      "VALUES((SELECT 1)),(2)",
      "SELECT 7 WHERE (SELECT abs(-9223372036854775808)) NOT IN (?1)",
      "SELECT id,count() FROM Items GROUP BY id "
      "HAVING id>0 AND (SELECT 1) AND count()>0",
      "SELECT id>0 AND count() AS a FROM Items GROUP BY id HAVING a AND count()>0",
      "SELECT 1 AS yes FROM Items a LEFT JOIN Items b ON +yes AND a.id IN (1)",
      "SELECT a.id,count(*) FROM Items a LEFT JOIN Items b ON a.id=b.id "
      "WHERE a.id>0 GROUP BY a.id HAVING a.id<5",
      "SELECT 1 FROM Items a JOIN Items b ON a.id=b.id LEFT JOIN Items c "
      "ON (SELECT x.id FROM Items x FULL JOIN Items y USING(id,Name) LIMIT 1)",
      "UPDATE Items SET Name=(SELECT max(a.Name) FROM Items a LEFT JOIN Items b "
      "ON a.id=b.id) WHERE id=?1",
      "SELECT 1 FROM Items a LEFT JOIN Items b ON EXISTS(SELECT * FROM Items x)",
      "SELECT c.id AS later FROM Items a LEFT JOIN Items b ON later CROSS JOIN Items c",
      "SELECT (SELECT c.id) AS later FROM Items a LEFT JOIN Items b ON later CROSS JOIN Items c",
      "SELECT 0 AS no FROM Items a FULL JOIN Items b ON no AND 1",
      "SELECT 1 FROM Items a LEFT JOIN Items b "
      "ON 1 IN (coalesce(abs(a.id) OR 1,0),2) AND a.id>0",
      "SELECT 1 AS yes FROM Items a LEFT JOIN Items b ON 1 IN ((SELECT yes OR 1),2)",
      "SELECT ?1 IN (abs(abs(abs(?2+?3+?4+?5))),0)",
      "SELECT abs(abs(abs(?1+?2+?3+?4))) AS x "
      "ORDER BY abs(abs(abs(?1+?2+?3+?4)))",
      "SELECT group_concat(abs(?1+?2+?3+?4) ORDER BY abs(?1+?2+?3+?4)),"
      "group_concat(abs(?1+?2+?3+?4) ORDER BY abs(?1+?2+?3+?4)) FROM Items",
      "SELECT abs(abs(abs((SELECT ?1)+?2+?3))) AS x FROM Items WHERE x>0",
      "SELECT count() FROM Items GROUP BY ?1 HAVING abs(abs(abs(?1+?2+?3+?4)))",
      "SELECT (SELECT sum(abs(abs(abs(o.id+(SELECT 1)+?1+?2))))) FROM Items o",
      "VALUES(1 COLLATE NOCASE),(2)",
      "SELECT EXISTS(VALUES(1 COLLATE NOCASE),(2)),"
      "EXISTS(VALUES(3 COLLATE RTRIM),(4)),EXISTS(VALUES(5 COLLATE BINARY),(6))",
      "SELECT (VALUES(1),(2),((SELECT EXISTS(VALUES(3),(4)))),(5),(6))",
      "SELECT (VALUES(abs(7)),(8),(9 IS 9),(10)) AS v FROM Items WHERE v",
      "SELECT 1 OR (VALUES((abs(-9223372036854775808) COLLATE BINARY+0)+0),(1))",
      "SELECT 0 AND (VALUES((abs(-9223372036854775808) COLLATE BINARY+0)+0),(1))",
      "SELECT 1 COLLATE missing",
      "SELECT EXISTS(VALUES('a' COLLATE NOCASE),('b')) AS v FROM Items ORDER BY 1,v,1",
      "SELECT EXISTS(SELECT 1 ORDER BY (VALUES('a' COLLATE NOCASE),('b')))",
      "UPDATE Items SET Name=(VALUES('a' COLLATE NOCASE),('b')),"
      "Name=(VALUES('c' COLLATE RTRIM),('d')) WHERE id=?1",
      "INSERT INTO DefaultOwner DEFAULT VALUES",
      "INSERT INTO DefaultOwner(a,a,deep) VALUES(?1,?2,?3)",
      "INSERT INTO Items(Name,name) VALUES(?1,(SELECT sum((SELECT 1,2))))",
      "CREATE TABLE DeclaredDefaults("
      "a TEXT DEFAULT 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789',"
      "b BLOB DEFAULT X'00112233445566778899AABBCCDDEEFF',"
      "c DEFAULT(0x10000000000000000) DEFAULT 2,d DEFAULT(-'5.0'),e DEFAULT(-NULL))",
      "CREATE TABLE BooleanDefaults(FALSE INTEGER,a DEFAULT(-FALSE),b DEFAULT \"FALSE\")",
      "INSERT INTO UnaryDefaults DEFAULT VALUES",
      "INSERT INTO UnaryDefaults(a,c) VALUES(?1,NULL)",
  };
  const auto full_sweep = [&](std::string_view full_sweep_sql,
                              std::optional<BindErrorCode> expected_error = std::nullopt,
                              bool expected_value_requirement = false) {
    SyntaxTree aggregate_baseline_tree = ParseTree(full_sweep_sql);
    allocation_index.store(0, std::memory_order_relaxed);
    const BindStatementResult aggregate_baseline =
        BindStatement(std::move(aggregate_baseline_tree), catalog);
    if (expected_error.has_value()) {
      if (aggregate_baseline.has_value() || aggregate_baseline.error().code != *expected_error ||
          aggregate_baseline.error().value_requirement != expected_value_requirement) {
        return false;
      }
    } else if (!aggregate_baseline.has_value()) {
      return false;
    }
    const std::size_t aggregate_allocations = allocation_index.load(std::memory_order_relaxed);
    if (aggregate_allocations == 0U || aggregate_allocations > 1024U) {
      return false;
    }
    for (std::size_t failure = 0; failure < aggregate_allocations; ++failure) {
      SyntaxTree tree = ParseTree(full_sweep_sql);
      allocation_index.store(0, std::memory_order_relaxed);
      failing_allocation = failure;
      inject_failure = true;
      bool threw = false;
      try {
        [[maybe_unused]] const BindStatementResult unexpected =
            BindStatement(std::move(tree), catalog);
      } catch (const std::bad_alloc&) {
        threw = true;
      }
      inject_failure = false;
      if (!threw) {
        return false;
      }
    }
    const BindStatementResult recovered = BindStatement(ParseTree(full_sweep_sql), catalog);
    return expected_error.has_value()
               ? !recovered.has_value() && recovered.error().code == *expected_error &&
                     recovered.error().value_requirement == expected_value_requirement
               : recovered.has_value();
  };
  for (const std::string_view full_sweep_sql : full_sweep_sqls) {
    if (!full_sweep(full_sweep_sql)) {
      return 1;
    }
  }
  for (const std::string_view sql : {
           "VALUES('a' COLLATE first_missing,'b' COLLATE second_missing),('c','d')",
           "SELECT EXISTS(VALUES('a' COLLATE first_missing),('b')) AS v FROM Items "
           "ORDER BY EXISTS(VALUES('c' COLLATE second_missing),('d')),v,1",
       }) {
    if (!full_sweep(sql, BindErrorCode::kNoSuchCollation)) {
      return 1;
    }
  }
  if (!full_sweep("INSERT INTO InvalidDefaults DEFAULT VALUES", BindErrorCode::kInvalidLiteral,
                  true)) {
    return 1;
  }
  if (!full_sweep("CREATE TABLE InvalidDefault(a DEFAULT 1 DEFAULT(\"FALSE\"))",
                  BindErrorCode::InvalidDefault)) {
    return 1;
  }
  if (!full_sweep("INSERT INTO InvalidNegativeDefaults DEFAULT VALUES",
                  BindErrorCode::kInvalidLiteral, true)) {
    return 1;
  }

  std::string compared_expression = "id";
  for (std::size_t index = 0; index < 32U; ++index) {
    compared_expression += "+id";
  }
  const std::string comparison_sql =
      "SELECT " + compared_expression + "," + compared_expression + " FROM Items";
  const auto comparison_owner = BindSelectStatement(ParseTree(comparison_sql), catalog);
  if (!comparison_owner.has_value() || comparison_owner->result_columns().size() != 2U) {
    return 1;
  }
  const auto comparison_results = comparison_owner->result_columns();
  const auto compare = [&] {
    return NativeBoundExpressionsEquivalent(*comparison_owner, comparison_results[0].expression,
                                            comparison_results[1].expression);
  };
  allocation_index.store(0, std::memory_order_relaxed);
  const auto baseline_comparison = compare();
  const std::size_t comparison_allocations = allocation_index.load(std::memory_order_relaxed);
  if (!baseline_comparison.has_value() || !*baseline_comparison || comparison_allocations == 0U ||
      comparison_allocations > 1024U) {
    return 1;
  }
  for (std::size_t failure = 0; failure < comparison_allocations; ++failure) {
    allocation_index.store(0, std::memory_order_relaxed);
    failing_allocation = failure;
    inject_failure = true;
    bool threw = false;
    try {
      [[maybe_unused]] const auto unexpected = compare();
    } catch (const std::bad_alloc&) {
      threw = true;
    }
    inject_failure = false;
    if (!threw) {
      return 1;
    }
    const auto recovered = compare();
    if (!recovered.has_value() || !*recovered ||
        comparison_owner->source().bytes() != comparison_sql) {
      return 1;
    }
  }

  const auto integer_owner =
      BindSelectStatement(ParseTree("SELECT 0,0.0,+0,0xffffffffffffffff,?1"), catalog);
  if (!integer_owner.has_value() || integer_owner->result_columns().size() != 5U) {
    return 1;
  }
  const auto integer_results = integer_owner->result_columns();
  allocation_index.store(0, std::memory_order_relaxed);
  failing_allocation = 0;
  inject_failure = true;
  for (std::size_t index = 0; index < integer_results.size(); ++index) {
    const auto matched =
        NativeBoundExpressionMatchesInteger(*integer_owner, integer_results[index].expression, 0);
    if (!matched.has_value() || *matched != (index == 0U)) {
      inject_failure = false;
      return 1;
    }
  }
  inject_failure = false;
  if (allocation_index.load(std::memory_order_relaxed) != 0U) {
    return 1;
  }

  return 0;
} catch (...) {
  inject_failure = false;
  return 1;
}
