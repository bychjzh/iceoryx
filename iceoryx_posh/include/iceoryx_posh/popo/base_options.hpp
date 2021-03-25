// Copyright (c) 2021 by Apex.AI Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef IOX_POSH_POPO_BASE_OPTIONS_HPP
#define IOX_POSH_POPO_BASE_OPTIONS_HPP

#include <cstdint>

namespace iox
{
namespace popo
{
enum class QueueFullPolicy : uint8_t
{
    BLOCK_PUBLISHER,
    DISCARD_OLDEST_DATA
};
}
} // namespace iox
#endif // IOX_POSH_POPO_PUBLISHER_OPTIONS_HPP

