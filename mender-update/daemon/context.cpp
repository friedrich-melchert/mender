// Copyright 2023 Northern.tech AS
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//
//    Unless required by applicable law or agreed to in writing, software
//    distributed under the License is distributed on an "AS IS" BASIS,
//    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//    See the License for the specific language governing permissions and
//    limitations under the License.

#include <mender-update/daemon/context.hpp>

#include <common/common.hpp>
#include <common/log.hpp>

namespace mender {
namespace update {
namespace daemon {

namespace common = mender::common;
namespace log = mender::common::log;
namespace main_context = mender::update::context;

const int kStateDataVersion = 2;

// The maximum times we are allowed to move through update states. If this is exceeded then the
// update will be forcefully aborted. This can happen if we are in a reboot loop, for example.
const int kMaxStateDataStoreCount = 50;

ExpectedStateData ApiResponseJsonToStateData(const json::Json &json) {
	StateData data;

	expected::ExpectedString str = json.Get("id").and_then(json::ToString);
	if (!str) {
		return expected::unexpected(str.error().WithContext("Could not get deployment ID"));
	}
	data.update_info.id = str.value();

	str = json.Get("artifact")
			  .and_then([](const json::Json &json) { return json.Get("source"); })
			  .and_then([](const json::Json &json) { return json.Get("uri"); })
			  .and_then(json::ToString);
	if (!str) {
		return expected::unexpected(
			str.error().WithContext("Could not get artifact URI for deployment"));
	}
	data.update_info.artifact.source.uri = str.value();
	log::Debug("Artifact Download URL: " + data.update_info.artifact.source.uri);

	str = json.Get("artifact")
			  .and_then([](const json::Json &json) { return json.Get("source"); })
			  .and_then([](const json::Json &json) { return json.Get("expire"); })
			  .and_then(json::ToString);
	if (str) {
		data.update_info.artifact.source.expire = str.value();
		// If it's not available, we don't care.
	}

	// For later: Update Control Maps should be handled here.

	// Note: There is more information available in the response than we collect here, but we
	// prefer to get the information from the artifact instead, since it is the authoritative
	// source. And it's also signed, unlike the response.

	return data;
}

const string Context::kRollbackNotSupported = "rollback-not-supported";
const string Context::kRollbackSupported = "rollback-supported";

string SupportsRollbackToDbString(bool support) {
	return support ? Context::kRollbackSupported : Context::kRollbackNotSupported;
}

expected::ExpectedBool DbStringToSupportsRollback(const string &str) {
	if (str == Context::kRollbackSupported) {
		return true;
	} else if (str == Context::kRollbackNotSupported) {
		return false;
	} else {
		return expected::unexpected(main_context::MakeError(
			main_context::DatabaseValueError,
			"\"" + str + "\" is not a valid value for SupportsRollback"));
	}
}

const string Context::kRebootTypeNone = "";
const string Context::kRebootTypeCustom = "reboot-type-custom";
const string Context::kRebootTypeAutomatic = "reboot-type-automatic";

string NeedsRebootToDbString(update_module::RebootAction action) {
	switch (action) {
	case update_module::RebootAction::No:
		return Context::kRebootTypeNone;
	case update_module::RebootAction::Automatic:
		return Context::kRebootTypeAutomatic;
	case update_module::RebootAction::Yes:
		return Context::kRebootTypeCustom;
	default:
		// Should not happen.
		assert(false);
		return Context::kRebootTypeNone;
	}
}

update_module::ExpectedRebootAction DbStringToNeedsReboot(const string &str) {
	if (str == Context::kRebootTypeNone) {
		return update_module::RebootAction::No;
	} else if (str == Context::kRebootTypeAutomatic) {
		return update_module::RebootAction::Automatic;
	} else if (str == Context::kRebootTypeCustom) {
		return update_module::RebootAction::Yes;
	} else {
		return expected::unexpected(main_context::MakeError(
			main_context::DatabaseValueError,
			"\"" + str + "\" is not a valid value for RebootRequested"));
	}
}

void StateData::FillUpdateDataFromArtifact(artifact::PayloadHeaderView &view) {
	version = view.version;
	auto &artifact = update_info.artifact;
	auto &header = view.header;
	artifact.compatible_devices = header.header_info.depends.device_type;
	artifact.payload_types = {header.payload_type};
	artifact.artifact_name = header.artifact_name;
	artifact.artifact_group = header.artifact_group;
	if (header.type_info.artifact_provides) {
		artifact.type_info_provides = header.type_info.artifact_provides.value();
	} else {
		artifact.type_info_provides.clear();
	}
	if (header.type_info.clears_artifact_provides) {
		artifact.clears_artifact_provides = header.type_info.clears_artifact_provides.value();
	} else {
		artifact.clears_artifact_provides.clear();
	}
}

Context::Context(main_context::MenderContext &mender_context, events::EventLoop &event_loop) :
	mender_context(mender_context),
	event_loop(event_loop),
	deployment_client(http::ClientConfig(mender_context.GetConfig().server_url), event_loop),
	download_client(http::ClientConfig(mender_context.GetConfig().server_url), event_loop) {
}

static string GenerateStateDataJson(const StateData &state_data) {
	stringstream content;

	auto append_vector = [&content](const vector<string> &data) {
		for (auto entry = data.begin(); entry != data.end(); entry++) {
			if (entry != data.begin()) {
				content << ",";
			}
			content << R"(")" << json::EscapeString(*entry) << R"(")";
		}
	};

	auto append_map = [&content](const unordered_map<string, string> &data) {
		for (auto entry = data.begin(); entry != data.end(); entry++) {
			if (entry != data.begin()) {
				content << ",";
			}
			content << R"(")" << json::EscapeString(entry->first) << R"(":")"
					<< json::EscapeString(entry->second) << R"(")";
		}
	};

	content << "{";
	{
		content << R"("Version":)" << to_string(state_data.version) << ",";
		content << R"("Name":")" << json::EscapeString(state_data.state) << R"(",)";
		content << R"("UpdateInfo":{)";
		{
			auto &update_info = state_data.update_info;
			content << R"("Artifact":{)";
			{
				auto &artifact = update_info.artifact;
				content << R"("Source":{)";
				{
					content << R"("URI":")" << json::EscapeString(artifact.source.uri) << R"(",)";
					content << R"("Expire":")" << json::EscapeString(artifact.source.expire)
							<< R"(")";
				}
				content << "},";

				content << R"("CompatibleDevices":[)";
				append_vector(artifact.compatible_devices);
				content << "],";

				content << R"("PayloadTypes":[)";
				append_vector(artifact.payload_types);
				content << "],";

				content << R"("ArtifactName":")" << json::EscapeString(artifact.artifact_name)
						<< R"(",)";
				content << R"("ArtifactGroup":")" << json::EscapeString(artifact.artifact_group)
						<< R"(",)";

				content << R"("TypeInfoProvides":{)";
				append_map(artifact.type_info_provides);
				content << "},";

				content << R"("ClearsArtifactProvides":[)";
				append_vector(artifact.clears_artifact_provides);
				content << "]";
			}
			content << "},";

			content << R"("ID":")" << json::EscapeString(update_info.id) << R"(",)";

			content << R"("RebootRequested":[)";
			append_vector(update_info.reboot_requested);
			content << R"(],)";

			content << R"("SupportsRollback":")"
					<< json::EscapeString(update_info.supports_rollback) << R"(",)";
			content << R"("StateDataStoreCount":)" << to_string(update_info.state_data_store_count)
					<< R"(,)";
			content << R"("HasDBSchemaUpdate":)"
					<< string(update_info.has_db_schema_update ? "true" : "false");
		}
		content << "}";
	}
	content << "}";

	return std::move(*content.rdbuf()).str();
}

error::Error Context::SaveDeploymentStateData(kv_db::Transaction &txn, StateData &state_data) {
	// We do not currently support this being set to true. It is only relevant if upgrading from
	// a client older than 2.0. It can become relevant again later if we upgrade the schema
	// version. See also comment about state_data_key_uncommitted.
	AssertOrReturnError(!state_data.update_info.has_db_schema_update);

	if (state_data.update_info.state_data_store_count++ >= kMaxStateDataStoreCount) {
		return main_context::MakeError(
			main_context::StateDataStoreCountExceededError,
			"State looping detected, breaking out of loop");
	}

	string content = GenerateStateDataJson(state_data);

	// TODO: Handle commit stage here.
	string store_key;
	if (state_data.update_info.has_db_schema_update) {
		store_key = mender_context.state_data_key_uncommitted;
	} else {
		store_key = mender_context.state_data_key;
	}

	auto err = txn.Write(store_key, common::ByteVectorFromString(content));
	if (err != error::NoError) {
		return err.WithContext("Could not write state data");
	}

	return error::NoError;
}

error::Error Context::SaveDeploymentStateData(StateData &state_data) {
	auto &db = mender_context.GetMenderStoreDB();
	return db.WriteTransaction([this, &state_data](kv_db::Transaction &txn) {
		return SaveDeploymentStateData(txn, state_data);
	});
}

static error::Error UnmarshalJsonStateData(const json::Json &json, StateData &state_data) {
#define SetOrReturnIfError(dst, expr) \
	if (!expr) {                      \
		return expr.error();          \
	}                                 \
	dst = expr.value()

#define EmptyOrSetOrReturnIfError(dst, expr)                                   \
	if (!expr) {                                                               \
		if (expr.error().code == kv_db::MakeError(kv_db::KeyError, "").code) { \
			dst.clear();                                                       \
		} else {                                                               \
			return expr.error();                                               \
		}                                                                      \
	} else {                                                                   \
		dst = expr.value();                                                    \
	}

	auto exp_int = json.Get("Version").and_then(json::ToInt);
	SetOrReturnIfError(state_data.version, exp_int);

	if (state_data.version != kStateDataVersion) {
		return error::Error(
			make_error_condition(errc::not_supported),
			"State Data version not supported by this client");
	}

	auto exp_string = json.Get("Name").and_then(json::ToString);
	SetOrReturnIfError(state_data.state, exp_string);

	const auto &exp_json_update_info = json.Get("UpdateInfo");
	SetOrReturnIfError(const auto &json_update_info, exp_json_update_info);

	const auto &exp_json_artifact = json_update_info.Get("Artifact");
	SetOrReturnIfError(const auto &json_artifact, exp_json_artifact);

	const auto &exp_json_source = json_artifact.Get("Source");
	SetOrReturnIfError(const auto &json_source, exp_json_source);

	auto &update_info = state_data.update_info;
	auto &artifact = update_info.artifact;
	auto &source = artifact.source;

	exp_string = json_source.Get("URI").and_then(json::ToString);
	SetOrReturnIfError(source.uri, exp_string);

	exp_string = json_source.Get("Expire").and_then(json::ToString);
	SetOrReturnIfError(source.expire, exp_string);

	auto exp_string_vector = json_artifact.Get("CompatibleDevices").and_then(json::ToStringVector);
	SetOrReturnIfError(artifact.compatible_devices, exp_string_vector);

	exp_string_vector = json_artifact.Get("PayloadTypes").and_then(json::ToStringVector);
	SetOrReturnIfError(artifact.payload_types, exp_string_vector);

	exp_string = json_artifact.Get("ArtifactName").and_then(json::ToString);
	SetOrReturnIfError(artifact.artifact_name, exp_string);

	exp_string = json_artifact.Get("ArtifactGroup").and_then(json::ToString);
	SetOrReturnIfError(artifact.artifact_group, exp_string);

	auto exp_string_map = json_artifact.Get("TypeInfoProvides").and_then(json::ToKeyValuesMap);
	EmptyOrSetOrReturnIfError(artifact.type_info_provides, exp_string_map);

	exp_string_vector = json_artifact.Get("ClearsArtifactProvides").and_then(json::ToStringVector);
	EmptyOrSetOrReturnIfError(artifact.clears_artifact_provides, exp_string_vector);

	exp_string = json_update_info.Get("ID").and_then(json::ToString);
	SetOrReturnIfError(update_info.id, exp_string);

	exp_string_vector = json_update_info.Get("RebootRequested").and_then(json::ToStringVector);
	SetOrReturnIfError(update_info.reboot_requested, exp_string_vector);

	auto exp_bool = json_update_info.Get("SupportsRollback").and_then(json::ToBool);
	SetOrReturnIfError(update_info.supports_rollback, exp_bool);

	exp_int = json_update_info.Get("StateDataStoreCount").and_then(json::ToInt);
	SetOrReturnIfError(update_info.state_data_store_count, exp_int);

	exp_bool = json_update_info.Get("HasDBSchemaUpdate").and_then(json::ToBool);
	SetOrReturnIfError(update_info.has_db_schema_update, exp_bool);

#undef SetOrReturnIfError
#undef EmptyOrSetOrReturnIfError

	return error::NoError;
}

expected::ExpectedBool Context::LoadDeploymentStateData(StateData &state_data) {
	auto &db = mender_context.GetMenderStoreDB();
	auto err = db.WriteTransaction([this, &state_data](kv_db::Transaction &txn) {
		auto exp_content = txn.Read(mender_context.state_data_key);
		if (!exp_content) {
			return exp_content.error().WithContext("Could not load state data");
		}
		auto &content = exp_content.value();

		auto exp_json = json::Load(common::StringFromByteVector(content));
		if (!exp_json) {
			return exp_json.error().WithContext("Could not load state data");
		}

		auto err = UnmarshalJsonStateData(exp_json.value(), state_data);
		if (err != error::NoError) {
			if (err.code != make_error_condition(errc::not_supported)) {
				return err.WithContext("Could not load state data");
			}

			// Try again with the state_data_key_uncommitted.
			exp_content = txn.Read(mender_context.state_data_key_uncommitted);
			if (!exp_content) {
				return err.WithContext("Could not load state data").FollowedBy(exp_content.error());
			}
			auto &content = exp_content.value();

			exp_json = json::Load(common::StringFromByteVector(content));
			if (!exp_json) {
				return err.WithContext("Could not load state data").FollowedBy(exp_json.error());
			}

			auto inner_err = UnmarshalJsonStateData(exp_json.value(), state_data);
			if (inner_err != error::NoError) {
				return err.WithContext("Could not load state data").FollowedBy(inner_err);
			}
		}

		// Every load also saves, which increments the state_data_store_count.
		return SaveDeploymentStateData(txn, state_data);
	});

	if (err == error::NoError) {
		return true;
	} else if (err.code == kv_db::MakeError(kv_db::KeyError, "").code) {
		return false;
	} else {
		return expected::unexpected(err);
	}
}

} // namespace daemon
} // namespace update
} // namespace mender
