#pragma once

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

class UnityCatalogExtension : public Extension {
public:
	void Load(ExtensionLoader &load) override;
	std::string Name() override;
};

} // namespace duckdb
