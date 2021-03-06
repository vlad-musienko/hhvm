/**
 * Autogenerated by Thrift for /home/fbthrift/thrift/lib/thrift/reflection.thrift
 *
 * DO NOT EDIT UNLESS YOU ARE SURE THAT YOU KNOW WHAT YOU ARE DOING
 *  @generated
 */
#pragma once

#include <thrift/lib/cpp2/gen/module_data_h.h>

#include "thrift/lib/thrift/gen-cpp2/reflection_types.h"

namespace apache { namespace thrift {

template <> struct TEnumDataStorage<::apache::thrift::reflection::Type> {
  using type = ::apache::thrift::reflection::Type;
  static constexpr const std::size_t size = 16;
  static constexpr const std::array<type, size> values = {{
    type::TYPE_VOID,
    type::TYPE_STRING,
    type::TYPE_BOOL,
    type::TYPE_BYTE,
    type::TYPE_I16,
    type::TYPE_I32,
    type::TYPE_I64,
    type::TYPE_DOUBLE,
    type::TYPE_ENUM,
    type::TYPE_LIST,
    type::TYPE_SET,
    type::TYPE_MAP,
    type::TYPE_STRUCT,
    type::TYPE_SERVICE,
    type::TYPE_PROGRAM,
    type::TYPE_FLOAT,
  }};
  static constexpr const std::array<folly::StringPiece, size> names = {{
    "TYPE_VOID",
    "TYPE_STRING",
    "TYPE_BOOL",
    "TYPE_BYTE",
    "TYPE_I16",
    "TYPE_I32",
    "TYPE_I64",
    "TYPE_DOUBLE",
    "TYPE_ENUM",
    "TYPE_LIST",
    "TYPE_SET",
    "TYPE_MAP",
    "TYPE_STRUCT",
    "TYPE_SERVICE",
    "TYPE_PROGRAM",
    "TYPE_FLOAT",
  }};
};


template <> struct TStructDataStorage<::apache::thrift::reflection::StructField> {
 private:
  using TType = apache::thrift::protocol::TType;

 public:
  static constexpr const std::size_t fields_size = 5;
  static constexpr std::array<folly::StringPiece, fields_size> fields_names = {{
    "isRequired",
    "type",
    "name",
    "annotations",
    "order",
  }};
  static constexpr std::array<int16_t, fields_size> fields_ids = {{
    1,
    2,
    3,
    4,
    5,
  }};
  static constexpr std::array<TType, fields_size> fields_types = {{
    TType::T_BOOL,
    TType::T_I64,
    TType::T_STRING,
    TType::T_MAP,
    TType::T_I16,
  }};
};


template <> struct TStructDataStorage<::apache::thrift::reflection::DataType> {
 private:
  using TType = apache::thrift::protocol::TType;

 public:
  static constexpr const std::size_t fields_size = 5;
  static constexpr std::array<folly::StringPiece, fields_size> fields_names = {{
    "name",
    "fields",
    "mapKeyType",
    "valueType",
    "enumValues",
  }};
  static constexpr std::array<int16_t, fields_size> fields_ids = {{
    1,
    2,
    3,
    4,
    5,
  }};
  static constexpr std::array<TType, fields_size> fields_types = {{
    TType::T_STRING,
    TType::T_MAP,
    TType::T_I64,
    TType::T_I64,
    TType::T_MAP,
  }};
};


template <> struct TStructDataStorage<::apache::thrift::reflection::Schema> {
 private:
  using TType = apache::thrift::protocol::TType;

 public:
  static constexpr const std::size_t fields_size = 2;
  static constexpr std::array<folly::StringPiece, fields_size> fields_names = {{
    "dataTypes",
    "names",
  }};
  static constexpr std::array<int16_t, fields_size> fields_ids = {{
    1,
    2,
  }};
  static constexpr std::array<TType, fields_size> fields_types = {{
    TType::T_MAP,
    TType::T_MAP,
  }};
};


}} // apache::thrift
