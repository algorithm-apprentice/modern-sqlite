#ifndef MODERN_SQLITE_CATALOG_CATALOG_LOADER_HPP_
#define MODERN_SQLITE_CATALOG_CATALOG_LOADER_HPP_

#include <cstddef>
#include <cstdint>
#include <string>

#include "modern_sqlite/base/result.hpp"
#include "modern_sqlite/catalog/catalog.hpp"
#include "modern_sqlite/pager/read_pager.hpp"

namespace modern_sqlite {

struct CatalogLoadOptions {
  std::string schema_name = "main";
  std::uint64_t generation = 0;
  std::size_t maximum_schema_rows = 10'000'000;
  std::size_t maximum_schema_objects = 10'000'000;
  std::size_t maximum_sql_bytes = 1'000'000'000;
  std::size_t maximum_columns = 2'000;
};

[[nodiscard]] Result<CatalogSnapshotPtr> LoadCatalog(ReadPager& pager);
[[nodiscard]] Result<CatalogSnapshotPtr> LoadCatalog(ReadPager& pager,
                                                     const CatalogLoadOptions& options);

[[nodiscard]] Result<bool> CatalogRequiresReload(const ReadPager& pager,
                                                 const CatalogSnapshot& catalog);

}  // namespace modern_sqlite

#endif  // MODERN_SQLITE_CATALOG_CATALOG_LOADER_HPP_
