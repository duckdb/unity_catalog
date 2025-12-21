#include "uc_utils.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "storage/uc_schema_entry.hpp"
#include "storage/uc_transaction.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "uc_api.hpp"

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
				children.push_back({child_name, child_type});
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

void UCTableCredentialManager::EnsureTableCredentials(ClientContext &context, const string &table_id,
                                                      const string &storage_location, const bool read_only,
                                                      const UCCredentials &credentials) {
	auto &secret_manager = SecretManager::Get(context);
	string secret_name = string(SECRET_NAME_PREFIX) + table_id;

	optional_ptr<UCTableCredentialCacheEntry> credential_cache_entry;
	idx_t current_expiration_time;
	bool read_only_cred;
	{
		lock_guard<mutex> lck(lock);
		auto res = entries.find(table_id);
		if (res == entries.end()) {
			entries[table_id] = make_uniq<UCTableCredentialCacheEntry>();
		}
		credential_cache_entry = entries[table_id];
		lock_guard<mutex> lck_table(credential_cache_entry->lock);
		read_only_cred = credential_cache_entry->read_only;
		current_expiration_time = credential_cache_entry->expiration_time;
	}

	// Check if secret exists and is still valid (not expired)
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
	auto existing_secret = secret_manager.GetSecretByName(transaction, secret_name, "memory");

	bool needs_refresh = true;
	if (existing_secret) {
		if (current_expiration_time) {
			auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
			    std::chrono::system_clock::now().time_since_epoch())
			                  .count();

			// Calculate time remaining until expiration (in milliseconds)
			int64_t time_remaining_ms = current_expiration_time - now_ms;

			// Refresh if expired or within safety margin of expiration
			// or the cached credential is read only and we need a write credential
			if (time_remaining_ms > REFRESH_SAFETY_MARGIN_MS && read_only_cred <= read_only) {
				needs_refresh = false;
			}
		}
	}

	if (needs_refresh) {
		// Get fresh credentials from UCAPI (includes expiration_time)
		auto table_credentials = UCAPI::GetTableCredentials(context, table_id, read_only, credentials);

		// Cache expiration time for future checks
		if (table_credentials.expiration_time > 0) {
			{
				lock_guard<mutex> lck(credential_cache_entry->lock);
				credential_cache_entry->expiration_time = table_credentials.expiration_time;
				credential_cache_entry->read_only = read_only;
			}
		}

		// Inject secret into secret manager scoped to this path
		CreateSecretInput input;
		input.on_conflict = OnCreateConflict::REPLACE_ON_CONFLICT;
		input.persist_type = SecretPersistType::TEMPORARY;
		input.name = secret_name;
		input.provider = "config";
		input.scope = {storage_location};
		// Starts with abfss: this is an azure blob storage location
		if (storage_location.find("abfss://") == 0) {
			// parse storage account name from location (abfss://catalog@account-name.dfs.windows.net/path-to-blob)
			auto storage_name_start = storage_location.find('@') + 1;
			auto storage_name_len = storage_location.find('.', storage_name_start) - storage_name_start;
			string storage_name = storage_location.substr(storage_name_start, storage_name_len);
			auto conn_str = "AccountName=" + storage_name + ";SharedAccessSignature=" + table_credentials.session_token;
			input.type = "azure";
			input.options = {{"connection_string", conn_str}};
		} else {
			input.type = "s3";
			input.options = {
			    {"key_id", table_credentials.key_id},
			    {"secret", table_credentials.secret},
			    {"session_token", table_credentials.session_token},
			    {"region", credentials.aws_region},
			};
		}

		secret_manager.CreateSecret(context, input);
	}
	// If secret exists and not expired, use cached secret
}

} // namespace duckdb
