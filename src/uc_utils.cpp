#include "uc_utils.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "yyjson.hpp"
#include "storage/uc_schema_entry.hpp"
#include "storage/uc_transaction.hpp"

#include <iostream>

namespace duckdb {

string UCUtils::TypeToString(const LogicalType &input) {
	switch (input.id()) {
	case LogicalType::VARCHAR:
		return "TEXT";
	case LogicalType::UTINYINT:
		return "TINYINT UNSIGNED";
	case LogicalType::USMALLINT:
		return "SMALLINT UNSIGNED";
	case LogicalType::UINTEGER:
		return "INTEGER UNSIGNED";
	case LogicalType::UBIGINT:
		return "BIGINT UNSIGNED";
	case LogicalType::TIMESTAMP:
		return "DATETIME";
	case LogicalType::TIMESTAMP_TZ:
		return "TIMESTAMP";
	default:
		return input.ToString();
	}
}

//===--------------------------------------------------------------------===//
// Types from the catalog's Delta StructField JSON
//===--------------------------------------------------------------------===//
//
// The same StructField JSON the Delta log carries, so nothing has to be split on separators that
// also occur inside a type (`decimal(10,2)`, a struct's children).

namespace {

using duckdb_yyjson::yyjson_val;

LogicalType TypeFromJsonValue(yyjson_val *type_val);

const char *ObjectString(yyjson_val *obj, const char *field) {
	auto *val = duckdb_yyjson::yyjson_obj_get(obj, field);
	if (!val || !duckdb_yyjson::yyjson_is_str(val)) {
		return nullptr;
	}
	return duckdb_yyjson::yyjson_get_str(val);
}

yyjson_val *ObjectField(yyjson_val *obj, const char *field) {
	auto *val = duckdb_yyjson::yyjson_obj_get(obj, field);
	if (!val) {
		throw NotImplementedException("Unity Catalog type JSON is missing the '%s' field", field);
	}
	return val;
}

// Delta's primitive names, not the SQL text spellings TypeToLogicalType takes.
LogicalType PrimitiveFromDeltaName(const string &name) {
	if (name == "string") {
		return LogicalType::VARCHAR;
	} else if (name == "long") {
		return LogicalType::BIGINT;
	} else if (name == "integer") {
		return LogicalType::INTEGER;
	} else if (name == "short") {
		return LogicalType::SMALLINT;
	} else if (name == "byte") {
		return LogicalType::TINYINT;
	} else if (name == "float") {
		return LogicalType::FLOAT;
	} else if (name == "double") {
		return LogicalType::DOUBLE;
	} else if (name == "boolean") {
		return LogicalType::BOOLEAN;
	} else if (name == "binary") {
		return LogicalType::BLOB;
	} else if (name == "date") {
		return LogicalType::DATE;
	} else if (name == "timestamp") {
		return LogicalType::TIMESTAMP_TZ;
	} else if (name == "timestamp_ntz") {
		return LogicalType::TIMESTAMP;
	} else if (name == "void") {
		return LogicalType::SQLNULL;
	} else if (name.find("decimal(") == 0) {
		auto spec_end = name.find(')');
		auto sep = name.find(',');
		if (spec_end == string::npos || sep == string::npos || sep > spec_end) {
			throw NotImplementedException("Malformed decimal in Unity Catalog type JSON: '%s'", name);
		}
		auto precision_str = name.substr(8, sep - 8);
		auto scale_str = name.substr(sep + 1, spec_end - sep - 1);
		return LogicalType::DECIMAL(Cast::Operation<string_t, uint8_t>(precision_str),
		                            Cast::Operation<string_t, uint8_t>(scale_str));
	}
	throw NotImplementedException("Unsupported type '%s' in Unity Catalog type JSON", name);
}

LogicalType StructFromJson(yyjson_val *struct_val) {
	child_list_t<LogicalType> children;
	size_t idx, max;
	yyjson_val *field;
	auto *fields = ObjectField(struct_val, "fields");
	yyjson_arr_foreach(fields, idx, max, field) {
		auto *name = ObjectString(field, "name");
		if (!name) {
			throw NotImplementedException("Unity Catalog type JSON has a struct field without a name");
		}
		children.emplace_back(name, TypeFromJsonValue(ObjectField(field, "type")));
	}
	if (children.empty()) {
		throw NotImplementedException("Unity Catalog type JSON has a struct with no fields");
	}
	return LogicalType::STRUCT(std::move(children));
}

LogicalType TypeFromJsonValue(yyjson_val *type_val) {
	if (duckdb_yyjson::yyjson_is_str(type_val)) {
		return PrimitiveFromDeltaName(duckdb_yyjson::yyjson_get_str(type_val));
	}
	if (!duckdb_yyjson::yyjson_is_obj(type_val)) {
		throw NotImplementedException("Unity Catalog type JSON is neither a type name nor a type object");
	}
	auto *kind = ObjectString(type_val, "type");
	if (!kind) {
		throw NotImplementedException("Unity Catalog type JSON object has no 'type'");
	}
	string kind_str = kind;
	if (kind_str == "struct") {
		return StructFromJson(type_val);
	} else if (kind_str == "array") {
		return LogicalType::LIST(TypeFromJsonValue(ObjectField(type_val, "elementType")));
	} else if (kind_str == "map") {
		return LogicalType::MAP(TypeFromJsonValue(ObjectField(type_val, "keyType")),
		                        TypeFromJsonValue(ObjectField(type_val, "valueType")));
	}
	throw NotImplementedException("Unsupported nested type '%s' in Unity Catalog type JSON", kind_str);
}

} // namespace

LogicalType UCUtils::TypeFromJson(ClientContext &context, const string &type_json) {
	auto *doc = duckdb_yyjson::yyjson_read(type_json.c_str(), type_json.size(), 0);
	if (!doc) {
		throw NotImplementedException("Unity Catalog returned unreadable type JSON: '%s'", type_json);
	}
	try {
		auto *root = duckdb_yyjson::yyjson_doc_get_root(doc);
		auto result = TypeFromJsonValue(ObjectField(root, "type"));
		duckdb_yyjson::yyjson_doc_free(doc);
		return result;
	} catch (...) {
		duckdb_yyjson::yyjson_doc_free(doc);
		throw;
	}
}

LogicalType UCUtils::ColumnTypeFromDefinition(ClientContext &context, const UCAPIColumnDefinition &column) {
	if (column.type_json.empty()) {
		return UCUtils::TypeToLogicalType(context, column.type_text);
	}
	return UCUtils::TypeFromJson(context, column.type_json);
}

LogicalType UCUtils::TypeToLogicalType(ClientContext &context, const string &type_text) {
	if (type_text == "tinyint") {
		return LogicalType::TINYINT;
	} else if (type_text == "smallint") {
		return LogicalType::SMALLINT;
	} else if (type_text == "bigint") {
		return LogicalType::BIGINT;
	} else if (type_text == "int") {
		return LogicalType::INTEGER;
	} else if (type_text == "long") {
		return LogicalType::BIGINT;
	} else if (type_text == "string" || type_text.find("varchar(") == 0 || type_text == "char" ||
	           type_text.find("char(") == 0) {
		return LogicalType::VARCHAR;
	} else if (type_text == "double") {
		return LogicalType::DOUBLE;
	} else if (type_text == "float") {
		return LogicalType::FLOAT;
	} else if (type_text == "boolean") {
		return LogicalType::BOOLEAN;
	} else if (type_text == "timestamp") {
		return LogicalType::TIMESTAMP_TZ;
	} else if (type_text == "binary") {
		return LogicalType::BLOB;
	} else if (type_text == "date") {
		return LogicalType::DATE;
	} else if (type_text == "void") {
		return LogicalType::SQLNULL; // TODO: This seems to be the closest match
	} else if (type_text.find("decimal(") == 0) {
		size_t spec_end = type_text.find(')');
		if (spec_end != string::npos) {
			size_t sep = type_text.find(',');
			auto prec_str = type_text.substr(8, sep - 8);
			auto scale_str = type_text.substr(sep + 1, spec_end - sep - 1);
			uint8_t prec = Cast::Operation<string_t, uint8_t>(prec_str);
			uint8_t scale = Cast::Operation<string_t, uint8_t>(scale_str);
			return LogicalType::DECIMAL(prec, scale);
		}
	} else if (type_text.find("array<") == 0) {
		size_t type_end = type_text.rfind('>'); // find last, to deal with nested
		if (type_end != string::npos) {
			auto child_type_str = type_text.substr(6, type_end - 6);
			auto child_type = UCUtils::TypeToLogicalType(context, child_type_str);
			return LogicalType::LIST(child_type);
		}
	} else if (type_text.find("map<") == 0) {
		size_t type_end = type_text.rfind('>'); // find last, to deal with nested
		if (type_end != string::npos) {
			// TODO: Factor this and struct parsing into an iterator over ',' separated values
			vector<LogicalType> key_val;
			size_t cur = 4;
			auto nested_opens = 0;
			for (;;) {
				size_t next_sep = cur;
				// find the location of the next ',' ignoring nested commas
				while (type_text[next_sep] != ',' || nested_opens > 0) {
					if (type_text[next_sep] == '<') {
						nested_opens++;
					} else if (type_text[next_sep] == '>') {
						nested_opens--;
					}
					next_sep++;
					if (next_sep == type_end) {
						break;
					}
				}
				auto child_str = type_text.substr(cur, next_sep - cur);
				auto child_type = UCUtils::TypeToLogicalType(context, child_str);
				key_val.push_back(child_type);
				if (next_sep == type_end) {
					break;
				}
				cur = next_sep + 1;
			}
			if (key_val.size() != 2) {
				throw NotImplementedException("Invalid map specification with %i types", key_val.size());
			}
			return LogicalType::MAP(key_val[0], key_val[1]);
		}
	} else if (type_text.find("struct<") == 0) {
		size_t type_end = type_text.rfind('>'); // find last, to deal with nested
		if (type_end != string::npos) {
			child_list_t<LogicalType> children;
			size_t cur = 7;
			auto nested_opens = 0;
			for (;;) {
				size_t next_sep = cur;
				// find the location of the next ',' ignoring nested commas
				while (type_text[next_sep] != ',' || nested_opens > 0) {
					if (type_text[next_sep] == '<') {
						nested_opens++;
					} else if (type_text[next_sep] == '>') {
						nested_opens--;
					}
					next_sep++;
					if (next_sep == type_end) {
						break;
					}
				}
				auto child_str = type_text.substr(cur, next_sep - cur);
				size_t type_sep = child_str.find(':');
				if (type_sep == string::npos) {
					throw NotImplementedException("Invalid struct child type specifier: %s", child_str);
				}
				auto child_name = child_str.substr(0, type_sep);
				auto child_type = UCUtils::TypeToLogicalType(context, child_str.substr(type_sep + 1, string::npos));
				children.emplace_back(child_name, child_type);
				if (next_sep == type_end) {
					break;
				}
				cur = next_sep + 1;
			}
			return LogicalType::STRUCT(children);
		}
	}

	throw NotImplementedException("Tried to fallback to unknown type for '%s'", type_text);
	// fallback for unknown types
	return LogicalType::VARCHAR;
}

LogicalType UCUtils::ToUCType(const LogicalType &input) {
	// todo do we need this mapping?
	throw NotImplementedException("ToUCType not yet implemented");
	switch (input.id()) {
	case LogicalTypeId::BOOLEAN:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::BLOB:
	case LogicalTypeId::DATE:
	case LogicalTypeId::DECIMAL:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::VARCHAR:
		return input;
	case LogicalTypeId::LIST:
		throw NotImplementedException("UC does not support arrays - unsupported type \"%s\"", input.ToString());
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::MAP:
	case LogicalTypeId::UNION:
		throw NotImplementedException("UC does not support composite types - unsupported type \"%s\"",
		                              input.ToString());
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
		return LogicalType::TIMESTAMP;
	case LogicalTypeId::HUGEINT:
		return LogicalType::DOUBLE;
	default:
		return LogicalType::VARCHAR;
	}
}

} // namespace duckdb
