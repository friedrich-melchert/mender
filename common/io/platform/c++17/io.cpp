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

#include <common/io.hpp>

#include <filesystem>

namespace mender {
namespace common {
namespace io {

namespace error = mender::common::error;
namespace expected = mender::common::expected;

namespace fs = std::filesystem;

expected::ExpectedSize FileSize(const string &path) {
	error_code ec;
	auto size = fs::file_size(fs::path(path), ec);
	if (ec) {
		return expected::unexpected(
			error::Error(ec.default_error_condition(), "Could not determine file size of " + path));
	}
	return size;
}

} // namespace io
} // namespace common
} // namespace mender
