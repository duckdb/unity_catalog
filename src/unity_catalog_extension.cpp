#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/storage/storage_extension.hpp"

#include "storage/unity_catalog.hpp"
#include "storage/uc_transaction_manager.hpp"
#include "functions/uc_checkpoint.hpp"
#include "uc_api.hpp"
#include "unity_catalog_extension.hpp"

namespace duckdb {

template <bool SHORT_NAME = false>
static unique_ptr<BaseSecret> CreateUCSecretFunction(ClientContext &, CreateSecretInput &input) {
	// apply any overridden settings
	vector<string> prefix_paths;

	string name;
	if (SHORT_NAME) {
		name = "uc";
	} else {
		name = "unity_catalog";
	}

	auto result = make_uniq<KeyValueSecret>(prefix_paths, name, "config", input.name);
	for (const auto &named_param : input.options) {
		auto lower_name = StringUtil::Lower(named_param.first);

		if (lower_name == "token") {
			result->secret_map["token"] = named_param.second.ToString();
		} else if (lower_name == "endpoint") {
			result->secret_map["endpoint"] = named_param.second.ToString();
		} else if (lower_name == "aws_region") {
			result->secret_map["aws_region"] = named_param.second.ToString();
		} else {
			throw InternalException("Unknown named parameter passed to CreateUCSecretFunction: " + lower_name);
		}
	}

	//! Set redact keys
	result->redact_keys = {"token"};

	return std::move(result);
}

static void SetUCSecretParameters(CreateSecretFunction &function) {
	function.named_parameters["token"] = LogicalType::VARCHAR;
	function.named_parameters["endpoint"] = LogicalType::VARCHAR;
	function.named_parameters["aws_region"] = LogicalType::VARCHAR;
}

unique_ptr<SecretEntry> GetSecret(ClientContext &context, const string &secret_name) {
	auto &secret_manager = SecretManager::Get(context);
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
	// FIXME: this should be adjusted once the `GetSecretByName` API supports this
	// use case
	auto secret_entry = secret_manager.GetSecretByName(transaction, secret_name, "memory");
	if (secret_entry) {
		return secret_entry;
	}
	secret_entry = secret_manager.GetSecretByName(transaction, secret_name, "local_file");
	if (secret_entry) {
		return secret_entry;
	}
	return nullptr;
}

template <bool DEPRECATED_NAME = false>
static unique_ptr<Catalog> UnityCatalogAttach(optional_ptr<StorageExtensionInfo> storage_info, ClientContext &context,
                                              AttachedDatabase &db, const string &name, AttachInfo &info,
                                              AttachOptions &attach_options) {
	UCCredentials credentials;

	// check if we have a secret provided
	string secret_name;
	string default_schema;
	for (auto &entry : info.options) {
		auto lower_name = StringUtil::Lower(entry.first);
		if (lower_name == "type" || lower_name == "read_only") {
			// already handled
		} else if (lower_name == "secret") {
			secret_name = entry.second.ToString();
		} else if (lower_name == "default_schema") {
			default_schema = entry.second.ToString();
		} else {
			throw BinderException("Unrecognized option for UC attach: %s", entry.first);
		}
	}

	// if no secret is specified we default to the unnamed mysql secret, if it
	// exists
	bool explicit_secret = !secret_name.empty();

	unique_ptr<SecretEntry> secret_entry;

	if (!secret_name.empty()) {
		secret_entry = GetSecret(context, secret_name);
	} else {
		// look up settings from the default unnamed secret if none is
		// provided
		secret_entry = GetSecret(context, "__default_unity_catalog");
		if (!secret_entry) {
			secret_entry = GetSecret(context, "__default_uc");
		}
	}

	string connection_string = info.path;
	if (secret_entry) {
		// secret found - read data
		const auto &kv_secret = dynamic_cast<const KeyValueSecret &>(*secret_entry->secret);
		string new_connection_info;

		Value input_val = kv_secret.TryGetValue("token");
		credentials.token = input_val.IsNull() ? "" : input_val.ToString();

		Value endpoint_val = kv_secret.TryGetValue("endpoint");
		credentials.endpoint = endpoint_val.IsNull() ? "" : endpoint_val.ToString();
		StringUtil::RTrim(credentials.endpoint, "/");

		Value aws_region_val = kv_secret.TryGetValue("aws_region");
		credentials.aws_region = aws_region_val.IsNull() ? "" : aws_region_val.ToString();

	} else if (explicit_secret) {
		// secret not found and one was explicitly provided - throw an error
		throw BinderException("Secret with name \"%s\" not found", secret_name);
	}

	if (default_schema.empty()) {
		//! No explicit default schema provided, ask the catalog:
		// Fixme: default namespace endpoint not available in OSS unity catalog, hence we throw
		try {
			default_schema = UCAPI::GetDefaultSchema(context, credentials);
		} catch (Exception &e) {
			DUCKDB_LOG_ERROR(context, "Failed to fetch default schema: %s", e.what());
		}
	}

	string catalog_name;
	if (DEPRECATED_NAME) {
		catalog_name = "uc_catalog";
	} else {
		catalog_name = "unity_catalog";
	}
	return make_uniq<UnityCatalog>(db, info.path, attach_options, credentials, default_schema, catalog_name);
}

static unique_ptr<TransactionManager> CreateTransactionManager(optional_ptr<StorageExtensionInfo> storage_info,
                                                               AttachedDatabase &db, Catalog &catalog) {
	auto &unity_catalog = catalog.Cast<UnityCatalog>();
	return make_uniq<UCTransactionManager>(db, unity_catalog);
}

template <bool DEPRECATED_NAME>
class UnityCatalogStorageExtension : public StorageExtension {
public:
	UnityCatalogStorageExtension() {
		attach = UnityCatalogAttach<DEPRECATED_NAME>;
		create_transaction_manager = CreateTransactionManager;
	}
};

static void LoadInternal(ExtensionLoader &loader) {
	// Register unity_catalog secret type
	SecretType secret_type;
	secret_type.name = "unity_catalog";
	secret_type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
	secret_type.default_provider = "config";
	loader.RegisterSecretType(secret_type);

	// Also register the short alias
	secret_type.name = "uc";
	loader.RegisterSecretType(secret_type);

	// Register the create secret function
	CreateSecretFunction mysql_secret_function = {"unity_catalog", "config", CreateUCSecretFunction};
	SetUCSecretParameters(mysql_secret_function);
	loader.RegisterFunction(mysql_secret_function);

	// Register the create secret function for the short alias
	CreateSecretFunction mysql_secret_function_deprecated = {"uc", "config", CreateUCSecretFunction<true>};
	SetUCSecretParameters(mysql_secret_function_deprecated);
	loader.RegisterFunction(mysql_secret_function_deprecated);

	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	auto extension = make_shared_ptr<UnityCatalogStorageExtension<false>>();
	StorageExtension::Register(config, "unity_catalog", extension);
	// Also register the (deprecated) alias
	StorageExtension::Register(config, "uc_catalog", extension);

	// Register table checkpoint functions
	loader.RegisterFunction(UCCheckpointTableFunction());
	loader.RegisterFunction(UCForceCheckpointTableFunction());
}

void UnityCatalogExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string UnityCatalogExtension::Name() {
	return "unity_catalog";
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(unity_catalog, loader) {
	duckdb::LoadInternal(loader);
}
}
