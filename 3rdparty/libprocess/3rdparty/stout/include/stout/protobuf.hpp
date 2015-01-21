/**
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef __STOUT_PROTOBUF_HPP__
#define __STOUT_PROTOBUF_HPP__

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>

#include <sys/types.h>

#include <glog/logging.h>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>

#include <google/protobuf/io/zero_copy_stream_impl.h>

#include <string>

#include <boost/lexical_cast.hpp>

#include "abort.hpp"
#include "error.hpp"
#include "json.hpp"
#include "none.hpp"
#include "os.hpp"
#include "result.hpp"
#include "stringify.hpp"
#include "try.hpp"

namespace protobuf {

// Write out the given protobuf to the specified file descriptor by
// first writing out the length of the protobuf followed by the
// contents.
// NOTE: On error, this may have written partial data to the file.
inline Try<Nothing> write(int fd, const google::protobuf::Message& message)
{
  if (!message.IsInitialized()) {
    return Error(message.InitializationErrorString() +
                 " is required but not initialized");
  }

  // First write the size of the protobuf.
  uint32_t size = message.ByteSize();
  std::string bytes((char*) &size, sizeof(size));

  Try<Nothing> result = os::write(fd, bytes);
  if (result.isError()) {
    return Error("Failed to write size: " + result.error());
  }

  if (!message.SerializeToFileDescriptor(fd)) {
    return Error("Failed to write/serialize message");
  }

  return Nothing();
}


// A wrapper function that wraps the above write with open and closing
// the file.
inline Try<Nothing> write(
    const std::string& path,
    const google::protobuf::Message& message)
{
  Try<int> fd = os::open(
      path,
      O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
      S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

  if (fd.isError()) {
    return Error("Failed to open file '" + path + "': " + fd.error());
  }

  Try<Nothing> result = write(fd.get(), message);

  // NOTE: We ignore the return value of close(). This is because
  // users calling this function are interested in the return value of
  // write(). Also an unsuccessful close() doesn't affect the write.
  os::close(fd.get());

  return result;
}


inline Try<Nothing> append(
    const std::string& path,
    const google::protobuf::Message& message)
{
  Try<int> fd = os::open(
      path,
      O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
      S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

  if (fd.isError()) {
    return Error("Failed to open file '" + path + "': " + fd.error());
  }

  Try<Nothing> result = write(fd.get(), message);

  // NOTE: We ignore the return value of close(). This is because
  // users calling this function are interested in the return value of
  // write(). Also an unsuccessful close() doesn't affect the write.
  os::close(fd.get());

  return result;
}


// Read the next protobuf of type T from the file by first reading the
// "size" followed by the contents (as written by 'write' above).
// If 'ignorePartial' is true, None() is returned when we unexpectedly
// hit EOF while reading the protobuf (e.g., partial write).
// If 'undoFailed' is true, failed read attempts will restore the file
// read/write file offset towards the initial callup position.
template <typename T>
inline Result<T> read(
    int fd,
    bool ignorePartial = false,
    bool undoFailed = false)
{
  off_t offset = 0;

  if (undoFailed) {
    // Save the offset so we can re-adjust if something goes wrong.
    offset = lseek(fd, 0, SEEK_CUR);
    if (offset == -1) {
      return ErrnoError("Failed to lseek to SEEK_CUR");
    }
  }

  uint32_t size;
  Result<std::string> result = os::read(fd, sizeof(size));

  if (result.isError()) {
    if (undoFailed) {
      lseek(fd, offset, SEEK_SET);
    }
    return Error("Failed to read size: " + result.error());
  } else if (result.isNone()) {
    return None(); // No more protobufs to read.
  } else if (result.get().size() < sizeof(size)) {
    // Hit EOF unexpectedly.
    if (undoFailed) {
      // Restore the offset to before the size read.
      lseek(fd, offset, SEEK_SET);
    }
    if (ignorePartial) {
      return None();
    }
    return Error(
        "Failed to read size: hit EOF unexpectedly, possible corruption");
  }

  // Parse the size from the bytes.
  memcpy((void*) &size, (void*) result.get().data(), sizeof(size));

  // NOTE: Instead of specifically checking for corruption in 'size',
  // we simply try to read 'size' bytes. If we hit EOF early, it is an
  // indication of corruption.
  result = os::read(fd, size);

  if (result.isError()) {
    if (undoFailed) {
      // Restore the offset to before the size read.
      lseek(fd, offset, SEEK_SET);
    }
    return Error("Failed to read message: " + result.error());
  } else if (result.isNone() || result.get().size() < size) {
    // Hit EOF unexpectedly.
    if (undoFailed) {
      // Restore the offset to before the size read.
      lseek(fd, offset, SEEK_SET);
    }
    if (ignorePartial) {
      return None();
    }
    return Error("Failed to read message of size " + stringify(size) +
                 " bytes: hit EOF unexpectedly, possible corruption");
  }

  // Parse the protobuf from the string.
  // NOTE: We need to capture a const reference to the data because it
  // must outlive the creation of ArrayInputStream.
  const std::string& data = result.get();

  T message;
  google::protobuf::io::ArrayInputStream stream(data.data(), data.size());

  if (!message.ParseFromZeroCopyStream(&stream)) {
    if (undoFailed) {
      // Restore the offset to before the size read.
      lseek(fd, offset, SEEK_SET);
    }
    return Error("Failed to deserialize message");
  }

  return message;
}


// A wrapper function that wraps the above read() with open and
// closing the file.
template <typename T>
inline Result<T> read(const std::string& path)
{
  Try<int> fd = os::open(
      path,
      O_RDONLY | O_CLOEXEC,
      S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

  if (fd.isError()) {
    return Error("Failed to open file '" + path + "': " + fd.error());
  }

  Result<T> result = read<T>(fd.get());

  // NOTE: We ignore the return value of close(). This is because
  // users calling this function are interested in the return value of
  // read(). Also an unsuccessful close() doesn't affect the read.
  os::close(fd.get());

  return result;
}


namespace internal {

// Forward declaration.
Try<Nothing> parse(
    google::protobuf::Message* message,
    const JSON::Object& object);


struct Parser : boost::static_visitor<Try<Nothing> >
{
  Parser(google::protobuf::Message* _message,
         const google::protobuf::FieldDescriptor* _field)
    : message(_message),
      reflection(message->GetReflection()),
      field(_field) {}

  Try<Nothing> operator () (const JSON::Object& object) const
  {
    switch (field->type()) {
      case google::protobuf::FieldDescriptor::TYPE_MESSAGE:
        if (field->is_repeated()) {
          parse(reflection->AddMessage(message, field), object);
        } else {
          parse(reflection->MutableMessage(message, field), object);
        }
        break;
      default:
        return Error("Not expecting a JSON object for field '" +
                     field->name() + "'");
    }
    return Nothing();
  }

  Try<Nothing> operator () (const JSON::String& string) const
  {
    switch (field->type()) {
      case google::protobuf::FieldDescriptor::TYPE_STRING:
      case google::protobuf::FieldDescriptor::TYPE_BYTES:
        if (field->is_repeated()) {
          reflection->AddString(message, field, string.value);
        } else {
          reflection->SetString(message, field, string.value);
        }
        break;
      case google::protobuf::FieldDescriptor::TYPE_ENUM: {
        const google::protobuf::EnumValueDescriptor* descriptor =
          field->enum_type()->FindValueByName(string.value);

        if (descriptor == NULL) {
          return Error("Failed to find enum for '" + string.value + "'");
        }

        if (field->is_repeated()) {
          reflection->AddEnum(message, field, descriptor);
        } else {
          reflection->SetEnum(message, field, descriptor);
        }
        break;
      }
      default:
        return Error("Not expecting a JSON string for field '" +
                     field->name() + "'");
    }
    return Nothing();
  }

  Try<Nothing> operator () (const JSON::Number& number) const
  {
    switch (field->type()) {
      case google::protobuf::FieldDescriptor::TYPE_DOUBLE:
        if (field->is_repeated()) {
          reflection->AddDouble(message, field, number.value);
        } else {
          reflection->SetDouble(message, field, number.value);
        }
        break;
      case google::protobuf::FieldDescriptor::TYPE_FLOAT:
        if (field->is_repeated()) {
          reflection->AddFloat(
              message,
              field,
              static_cast<float>(number.value));
        } else {
          reflection->SetFloat(
              message,
              field,
              static_cast<float>(number.value));
        }
        break;
      case google::protobuf::FieldDescriptor::TYPE_INT64:
      case google::protobuf::FieldDescriptor::TYPE_SINT64:
      case google::protobuf::FieldDescriptor::TYPE_SFIXED64:
        if (field->is_repeated()) {
          reflection->AddInt64(
              message,
              field,
              static_cast<int64_t>(number.value));
        } else {
          reflection->SetInt64(
              message,
              field,
              static_cast<int64_t>(number.value));
        }
        break;
      case google::protobuf::FieldDescriptor::TYPE_UINT64:
      case google::protobuf::FieldDescriptor::TYPE_FIXED64:
        if (field->is_repeated()) {
          reflection->AddUInt64(
              message,
              field,
              static_cast<uint64_t>(number.value));
        } else {
          reflection->SetUInt64(
              message,
              field,
              static_cast<uint64_t>(number.value));
        }
        break;
      case google::protobuf::FieldDescriptor::TYPE_INT32:
      case google::protobuf::FieldDescriptor::TYPE_SINT32:
      case google::protobuf::FieldDescriptor::TYPE_SFIXED32:
        if (field->is_repeated()) {
          reflection->AddInt32(
              message,
              field,
              static_cast<int32_t>(number.value));
        } else {
          reflection->SetInt32(
              message,
              field,
              static_cast<int32_t>(number.value));
        }
        break;
      case google::protobuf::FieldDescriptor::TYPE_UINT32:
      case google::protobuf::FieldDescriptor::TYPE_FIXED32:
        if (field->is_repeated()) {
          reflection->AddUInt32(
              message,
              field,
              static_cast<uint32_t>(number.value));
        } else {
          reflection->SetUInt32(
              message,
              field,
              static_cast<uint32_t>(number.value));
        }
        break;
      default:
        return Error("Not expecting a JSON number for field '" +
                     field->name() + "'");
    }
    return Nothing();
  }

  Try<Nothing> operator () (const JSON::Array& array) const
  {
    if (!field->is_repeated()) {
      return Error("Not expecting a JSON array for field '" +
                   field->name() + "'");
    }

    foreach (const JSON::Value& value, array.values) {
      Try<Nothing> apply =
        boost::apply_visitor(Parser(message, field), value);

      if (apply.isError()) {
        return Error(apply.error());
      }
    }

    return Nothing();
  }

  Try<Nothing> operator () (const JSON::Boolean& boolean) const
  {
    switch (field->type()) {
      case google::protobuf::FieldDescriptor::TYPE_BOOL:
        if (field->is_repeated()) {
          reflection->AddBool(message, field, boolean.value);
        } else {
          reflection->SetBool(message, field, boolean.value);
        }
        break;
      default:
        return Error("Not expecting a JSON boolean for field '" +
                     field->name() + "'");
    }
    return Nothing();
  }

  Try<Nothing> operator () (const JSON::Null&) const
  {
    return Error("Not expecting a JSON null");
  }

private:
  google::protobuf::Message* message;
  const google::protobuf::Reflection* reflection;
  const google::protobuf::FieldDescriptor* field;
};


inline Try<Nothing> parse(
    google::protobuf::Message* message,
    const JSON::Object& object)
{
  foreachpair (
      const std::string& name, const JSON::Value& value, object.values) {
    // Look for a field by this name.
    const google::protobuf::FieldDescriptor* field =
      message->GetDescriptor()->FindFieldByName(name);

    if (field != NULL) {
      Try<Nothing> apply =
        boost::apply_visitor(Parser(message, field), value);

      if (apply.isError()) {
        return Error(apply.error());
      }
    }
  }

  return Nothing();
}

} // namespace internal {


template <typename T>
Try<T> parse(const JSON::Value& value)
{
  { google::protobuf::Message* message = (T*) NULL; (void) message; }

  const JSON::Object* object = boost::get<JSON::Object>(&value);

  if (object == NULL) {
    return Error("Expecting a JSON object");
  }

  T message;

  Try<Nothing> parse = internal::parse(&message, *object);

  if (parse.isError()) {
    return Error(parse.error());
  }

  if (!message.IsInitialized()) {
    return Error("Missing required fields: " +
                 message.InitializationErrorString());
  }

  return message;
}

} // namespace protobuf {

namespace JSON {

struct Protobuf
{
  // TODO(bmahler): This currently uses the default value for optional
  // fields but we may want to revisit this decision.
  Protobuf(const google::protobuf::Message& message)
  {
    const google::protobuf::Descriptor* descriptor = message.GetDescriptor();
    const google::protobuf::Reflection* reflection = message.GetReflection();

    // We first look through all the possible fields to determine both
    // the set fields _and_ the optional fields with a default that
    // are not set. Reflection::ListFields() alone will only include
    // set fields and is therefore insufficient.
    std::vector<const google::protobuf::FieldDescriptor*> fields;
    for (int i = 0; i < descriptor->field_count(); i++) {
      const google::protobuf::FieldDescriptor* field = descriptor->field(i);
      if (field->is_repeated()) {
        if (reflection->FieldSize(message, descriptor->field(i)) > 0) {
          // Has repeated field with members, output as JSON.
          fields.push_back(field);
        }
      } else if (reflection->HasField(message, field) ||
                 field->has_default_value()) {
        // Field is set or has default, output as JSON.
        fields.push_back(field);
      }
    }

    foreach (const google::protobuf::FieldDescriptor* field, fields) {
      if (field->is_repeated()) {
        JSON::Array array;
        for (int i = 0; i < reflection->FieldSize(message, field); ++i) {
          switch (field->type()) {
            case google::protobuf::FieldDescriptor::TYPE_DOUBLE:
              array.values.push_back(JSON::Number(
                  reflection->GetRepeatedDouble(message, field, i)));
              break;
            case google::protobuf::FieldDescriptor::TYPE_FLOAT:
              array.values.push_back(JSON::Number(
                  reflection->GetRepeatedFloat(message, field, i)));
              break;
            case google::protobuf::FieldDescriptor::TYPE_INT64:
            case google::protobuf::FieldDescriptor::TYPE_SINT64:
            case google::protobuf::FieldDescriptor::TYPE_SFIXED64:
              array.values.push_back(JSON::Number(
                  reflection->GetRepeatedInt64(message, field, i)));
              break;
            case google::protobuf::FieldDescriptor::TYPE_UINT64:
            case google::protobuf::FieldDescriptor::TYPE_FIXED64:
              array.values.push_back(JSON::Number(
                  reflection->GetRepeatedUInt64(message, field, i)));
              break;
            case google::protobuf::FieldDescriptor::TYPE_INT32:
            case google::protobuf::FieldDescriptor::TYPE_SINT32:
            case google::protobuf::FieldDescriptor::TYPE_SFIXED32:
              array.values.push_back(JSON::Number(
                  reflection->GetRepeatedInt32(message, field, i)));
              break;
            case google::protobuf::FieldDescriptor::TYPE_UINT32:
            case google::protobuf::FieldDescriptor::TYPE_FIXED32:
              array.values.push_back(JSON::Number(
                  reflection->GetRepeatedUInt32(message, field, i)));
              break;
            case google::protobuf::FieldDescriptor::TYPE_BOOL:
              if (reflection->GetRepeatedBool(message, field, i)) {
                array.values.push_back(JSON::True());
              } else {
                array.values.push_back(JSON::False());
              }
              break;
            case google::protobuf::FieldDescriptor::TYPE_STRING:
            case google::protobuf::FieldDescriptor::TYPE_BYTES:
              array.values.push_back(JSON::String(
                  reflection->GetRepeatedString(message, field, i)));
              break;
            case google::protobuf::FieldDescriptor::TYPE_MESSAGE:
              array.values.push_back(Protobuf(
                  reflection->GetRepeatedMessage(message, field, i)));
              break;
            case google::protobuf::FieldDescriptor::TYPE_ENUM:
              array.values.push_back(JSON::String(
                  reflection->GetRepeatedEnum(message, field, i)->name()));
              break;
            case google::protobuf::FieldDescriptor::TYPE_GROUP:
              // Deprecated!
            default:
              ABORT("Unhandled protobuf field type: " +
                    stringify(field->type()));
          }
        }
        object.values[field->name()] = array;
      } else {
        switch (field->type()) {
          case google::protobuf::FieldDescriptor::TYPE_DOUBLE:
            object.values[field->name()] =
                JSON::Number(reflection->GetDouble(message, field));
            break;
          case google::protobuf::FieldDescriptor::TYPE_FLOAT:
            object.values[field->name()] =
                JSON::Number(reflection->GetFloat(message, field));
            break;
          case google::protobuf::FieldDescriptor::TYPE_INT64:
          case google::protobuf::FieldDescriptor::TYPE_SINT64:
          case google::protobuf::FieldDescriptor::TYPE_SFIXED64:
            object.values[field->name()] =
                JSON::Number(reflection->GetInt64(message, field));
            break;
          case google::protobuf::FieldDescriptor::TYPE_UINT64:
          case google::protobuf::FieldDescriptor::TYPE_FIXED64:
            object.values[field->name()] =
                JSON::Number(reflection->GetUInt64(message, field));
            break;
          case google::protobuf::FieldDescriptor::TYPE_INT32:
          case google::protobuf::FieldDescriptor::TYPE_SINT32:
          case google::protobuf::FieldDescriptor::TYPE_SFIXED32:
            object.values[field->name()] =
                JSON::Number(reflection->GetInt32(message, field));
            break;
          case google::protobuf::FieldDescriptor::TYPE_UINT32:
          case google::protobuf::FieldDescriptor::TYPE_FIXED32:
            object.values[field->name()] =
                JSON::Number(reflection->GetUInt32(message, field));
            break;
          case google::protobuf::FieldDescriptor::TYPE_BOOL:
            if (reflection->GetBool(message, field)) {
              object.values[field->name()] = JSON::True();
            } else {
              object.values[field->name()] = JSON::False();
            }
            break;
          case google::protobuf::FieldDescriptor::TYPE_STRING:
          case google::protobuf::FieldDescriptor::TYPE_BYTES:
            object.values[field->name()] =
                JSON::String(reflection->GetString(message, field));
            break;
          case google::protobuf::FieldDescriptor::TYPE_MESSAGE:
            object.values[field->name()] =
                Protobuf(reflection->GetMessage(message, field));
            break;
          case google::protobuf::FieldDescriptor::TYPE_ENUM:
            object.values[field->name()] =
                JSON::String(reflection->GetEnum(message, field)->name());
            break;
          case google::protobuf::FieldDescriptor::TYPE_GROUP:
            // Deprecated!
          default:
            ABORT("Unhandled protobuf field type: " +
                  stringify(field->type()));
        }
      }
    }
  }

  operator Object () const { return object; }

private:
  JSON::Object object;
};

} // namespace JSON {

#endif // __STOUT_PROTOBUF_HPP__
