/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __MESSAGES_LOG_HPP__
#define __MESSAGES_LOG_HPP__

#include <ostream>

#include "messages/log.pb.h"


// Definitions for operators on protobuf classes that don't have these
// operators generated by the protobuf compiler.

namespace mesos {
namespace internal {
namespace log {

inline std::ostream& operator<<(
    std::ostream& stream,
    const Action::Type& type)
{
  return stream << Action::Type_Name(type);
}


inline std::ostream& operator<<(
    std::ostream& stream,
    const Metadata::Status& status)
{
  return stream << Metadata::Status_Name(status);
}

} // namespace log {
} // namespace internal {
} // namespace mesos {

namespace std {

template <>
struct hash<mesos::internal::log::Metadata_Status>
{
  typedef std::size_t result_type;

  typedef mesos::internal::log::Metadata_Status argument_type;

  result_type operator()(const argument_type& status) const
  {
    // Use the underlying type of the enum as hash value.
    return static_cast<std::size_t>(status);
  }
};

} // namespace std {

#endif // __MESSAGES_LOG_HPP__
