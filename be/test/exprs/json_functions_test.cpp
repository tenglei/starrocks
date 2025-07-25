// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "exprs/json_functions.h"

#include <glog/logging.h>
#include <gtest/gtest-param-test.h>
#include <gtest/gtest.h>
#include <velocypack/vpack.h>

#include <string>
#include <vector>

#include "butil/time.h"
#include "column/column.h"
#include "column/const_column.h"
#include "column/map_column.h"
#include "column/nullable_column.h"
#include "column/struct_column.h"
#include "column/vectorized_fwd.h"
#include "common/config.h"
#include "common/status.h"
#include "common/statusor.h"
#include "exprs/mock_vectorized_expr.h"
#include "gtest/gtest-param-test.h"
#include "gutil/casts.h"
#include "gutil/strings/strip.h"
#include "testutil/assert.h"
#include "types/logical_type.h"
#include "util/defer_op.h"
#include "util/json.h"
#include "util/json_flattener.h"

namespace starrocks {

class JsonFunctionsTest : public ::testing::Test {
public:
    void SetUp() override {
        expr_node.opcode = TExprOpcode::ADD;
        expr_node.child_type = TPrimitiveType::INT;
        expr_node.node_type = TExprNodeType::BINARY_PRED;
        expr_node.num_children = 2;
        expr_node.__isset.opcode = true;
        expr_node.__isset.child_type = true;
        expr_node.type = gen_type_desc(TPrimitiveType::BOOLEAN);
    }

    Status test_extract_from_object(std::string input, const std::string& jsonpath, std::string* output) {
        // reverse for padding.
        input.reserve(input.size() + simdjson::SIMDJSON_PADDING);

        simdjson::ondemand::parser parser;
        simdjson::ondemand::document doc;
        EXPECT_EQ(simdjson::error_code::SUCCESS, parser.iterate(input).get(doc));

        simdjson::ondemand::object obj;

        EXPECT_EQ(simdjson::error_code::SUCCESS, doc.get_object().get(obj));

        std::vector<SimpleJsonPath> path;
        RETURN_IF_ERROR(JsonFunctions::parse_json_paths(jsonpath, &path));

        simdjson::ondemand::value val;
        RETURN_IF_ERROR(JsonFunctions::extract_from_object(obj, path, &val));
        std::string_view sv = simdjson::to_json_string(val);

        output->assign(sv.data(), sv.size());
        return Status::OK();
    }

public:
    TExprNode expr_node;
};

TEST_F(JsonFunctionsTest, get_json_string_casting) {
    std::unique_ptr<FunctionContext> ctx(FunctionContext::create_test_context());
    Columns columns;
    BinaryColumn::Ptr strings = BinaryColumn::create();
    BinaryColumn::Ptr strings2 = BinaryColumn::create();

    std::string values[] = {
            R"({"k1":    1})",          //int, 1 key
            R"({"k1":    1, "k2": 2})", // 2 keys, get the former
            R"({"k0":    0, "k1": 1})", // 2 keys, get  the latter

            R"({"k1":    3.14159})",                 //double, 1 key
            R"({"k0":    2.71828, "k1":  3.14159})", // 2 keys, get  the former
            R"({"k1":    3.14159, "k2":  2.71828})", // 2 keys, get  the latter

            R"({"k1":    "{\"k11\":       \"v11\"}"})",                                        //string, 1 key
            R"({"k0":    "{\"k01\":       \"v01\"}",  "k1":     "{\"k11\":       \"v11\"}"})", // 2 keys, get  the former
            R"({"k1":    "{\"k11\":       \"v11\"}",  "k2":     "{\"k21\": \"v21\"}"})", // 2 keys, get  the latter

            R"({"k1":    {"k11":       "v11"}})",                             //object, 1 key
            R"({"k0":    {"k01":       "v01"},  "k1":     {"k11": "v11"}})",  // 2 keys, get  the former
            R"({"k1":    {"k11":       "v11"},  "k2":     {"k21": "v21"}})"}; // 2 keys, get  the latter

    std::string strs[] = {"$.k1", "$.k1", "$.k1", "$.k1", "$.k1", "$.k1",
                          "$.k1", "$.k1", "$.k1", "$.k1", "$.k1", "$.k1"};
    std::string length_strings[] = {"1",
                                    "1",
                                    "1",
                                    "3.14159",
                                    "3.14159",
                                    "3.14159",
                                    R"({"k11":       "v11"})",
                                    R"({"k11":       "v11"})",
                                    R"({"k11":       "v11"})",
                                    R"({"k11": "v11"})",
                                    R"({"k11": "v11"})",
                                    R"({"k11": "v11"})"};

    for (int j = 0; j < sizeof(values) / sizeof(values[0]); ++j) {
        strings->append(values[j]);
        strings2->append(strs[j]);
    }

    columns.emplace_back(strings);
    columns.emplace_back(strings2);

    ctx.get()->set_constant_columns(columns);
    ASSERT_TRUE(JsonFunctions::native_json_path_prepare(ctx.get(), FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                        .ok());

    ColumnPtr result = JsonFunctions::get_json_string(ctx.get(), columns).value();

    auto v = ColumnHelper::cast_to<TYPE_VARCHAR>(result);

    for (int j = 0; j < sizeof(values) / sizeof(values[0]); ++j) {
        ASSERT_EQ(length_strings[j], v->get_data()[j].to_string());
    }

    ASSERT_TRUE(JsonFunctions::native_json_path_close(
                        ctx.get(), FunctionContext::FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                        .ok());
}

TEST_F(JsonFunctionsTest, get_json_string_array) {
    std::unique_ptr<FunctionContext> ctx(FunctionContext::create_test_context());
    Columns columns;
    BinaryColumn::Ptr strings = BinaryColumn::create();
    BinaryColumn::Ptr strings2 = BinaryColumn::create();

    std::string values[] = {R"([{"key":    1}, {"key": 2 }])", R"([{"key":    1}, {"key": 2 }])"};

    std::string strs[] = {"$[*].key", "$.[*].key"};
    std::string length_strings[] = {"[1, 2]", "[1, 2]"};

    for (int j = 0; j < sizeof(values) / sizeof(values[0]); ++j) {
        strings->append(values[j]);
        strings2->append(strs[j]);
    }

    columns.emplace_back(strings);
    columns.emplace_back(strings2);

    ctx.get()->set_constant_columns(columns);
    ASSERT_TRUE(JsonFunctions::native_json_path_prepare(ctx.get(), FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                        .ok());

    ColumnPtr result = JsonFunctions::get_json_string(ctx.get(), columns).value();

    auto v = ColumnHelper::cast_to<TYPE_VARCHAR>(result);

    for (int j = 0; j < sizeof(values) / sizeof(values[0]); ++j) {
        ASSERT_EQ(length_strings[j], v->get_data()[j].to_string());
    }

    ASSERT_TRUE(JsonFunctions::native_json_path_close(
                        ctx.get(), FunctionContext::FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                        .ok());
}

TEST_F(JsonFunctionsTest, get_json_emptyTest) {
    {
        std::unique_ptr<FunctionContext> ctx(FunctionContext::create_test_context());
        Columns columns;
        BinaryColumn::Ptr doubles = BinaryColumn::create();
        BinaryColumn::Ptr doubles2 = BinaryColumn::create();

        std::string values[] = {R"({"k1":1.3, "k2":"2"})", R"({"k1":"v1", "my.key":[1.1, 2.2, 3.3]})"};
        std::string strs[] = {"", ""};

        for (int j = 0; j < sizeof(values) / sizeof(values[0]); ++j) {
            doubles->append(values[j]);
            doubles2->append(strs[j]);
        }

        columns.emplace_back(doubles);
        columns.emplace_back(doubles2);

        ctx.get()->set_constant_columns(columns);
        ASSERT_TRUE(
                JsonFunctions::native_json_path_prepare(ctx.get(), FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                        .ok());

        ColumnPtr result = JsonFunctions::get_json_double(ctx.get(), columns).value();

        auto v = ColumnHelper::as_column<NullableColumn>(result);

        for (int j = 0; j < sizeof(values) / sizeof(values[0]); ++j) {
            ASSERT_TRUE(v->is_null(j));
        }

        ASSERT_TRUE(JsonFunctions::native_json_path_close(
                            ctx.get(), FunctionContext::FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                            .ok());
    }

    {
        std::unique_ptr<FunctionContext> ctx(FunctionContext::create_test_context());
        Columns columns;
        BinaryColumn::Ptr str_values = BinaryColumn::create();
        BinaryColumn::Ptr str_values2 = BinaryColumn::create();

        std::string values[] = {R"({"k1":1.3, "k2":"2"})", R"({"k1":"v1", "my.key":[1.1, 2.2, 3.3]})"};
        std::string strs[] = {"", ""};

        for (int j = 0; j < sizeof(values) / sizeof(values[0]); ++j) {
            str_values->append(values[j]);
            str_values2->append(strs[j]);
        }

        columns.emplace_back(str_values);
        columns.emplace_back(str_values2);

        ctx.get()->set_constant_columns(columns);
        ASSERT_TRUE(
                JsonFunctions::native_json_path_prepare(ctx.get(), FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                        .ok());

        ColumnPtr result = JsonFunctions::get_json_string(ctx.get(), columns).value();

        auto v = ColumnHelper::as_column<NullableColumn>(result);

        for (int j = 0; j < sizeof(values) / sizeof(values[0]); ++j) {
            ASSERT_TRUE(v->is_null(j));
        }

        ASSERT_TRUE(JsonFunctions::native_json_path_close(
                            ctx.get(), FunctionContext::FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                            .ok());
    }

    {
        std::unique_ptr<FunctionContext> ctx(FunctionContext::create_test_context());
        Columns columns;
        BinaryColumn::Ptr ints = BinaryColumn::create();
        BinaryColumn::Ptr ints2 = BinaryColumn::create();

        std::string values[] = {R"({"k1":1.3, "k2":"2"})", R"({"k1":"v1", "my.key":[1.1, 2.2, 3.3]})"};
        std::string strs[] = {"", ""};

        for (int j = 0; j < sizeof(values) / sizeof(values[0]); ++j) {
            ints->append(values[j]);
            ints2->append(strs[j]);
        }

        columns.emplace_back(ints);
        columns.emplace_back(ints2);

        ctx.get()->set_constant_columns(columns);
        ASSERT_TRUE(
                JsonFunctions::native_json_path_prepare(ctx.get(), FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                        .ok());

        ColumnPtr result = JsonFunctions::get_json_int(ctx.get(), columns).value();

        auto v = ColumnHelper::as_column<NullableColumn>(result);

        for (int j = 0; j < sizeof(values) / sizeof(values[0]); ++j) {
            ASSERT_TRUE(v->is_null(j));
        }

        ASSERT_TRUE(JsonFunctions::native_json_path_close(
                            ctx.get(), FunctionContext::FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                            .ok());
    }

    {
        std::unique_ptr<FunctionContext> ctx(FunctionContext::create_test_context());
        Columns columns;
        BinaryColumn::Ptr ints = BinaryColumn::create();
        BinaryColumn::Ptr ints2 = BinaryColumn::create();

        std::string values[] = {R"({"k1":1.3, "k2":"2"})", R"({"k1":"v1", "my.key":[1.1, 2.2, 3.3]})"};
        std::string strs[] = {"$.k3", "$.k4"};

        for (int j = 0; j < sizeof(values) / sizeof(values[0]); ++j) {
            ints->append(values[j]);
            ints2->append(strs[j]);
        }

        columns.emplace_back(ints);
        columns.emplace_back(ints2);

        ctx.get()->set_constant_columns(columns);
        ASSERT_TRUE(
                JsonFunctions::native_json_path_prepare(ctx.get(), FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                        .ok());

        ColumnPtr result = JsonFunctions::get_json_int(ctx.get(), columns).value();

        auto v = ColumnHelper::as_column<NullableColumn>(result);

        for (int j = 0; j < sizeof(values) / sizeof(values[0]); ++j) {
            ASSERT_TRUE(v->is_null(j));
        }

        ASSERT_TRUE(JsonFunctions::native_json_path_close(
                            ctx.get(), FunctionContext::FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                            .ok());
    }
}

// Test compatibility of float and double
TEST_F(JsonFunctionsTest, json_float_double) {
    namespace vpack = arangodb::velocypack;
    double x = 1.2;
    JsonValue json = JsonValue::from_double(static_cast<double>(x));
    vpack::Slice vslice = json.to_vslice();
    ASSERT_DOUBLE_EQ(1.2, vslice.getDouble());

    // check to_string
    auto json_str = json.to_string();
    ASSERT_TRUE(json_str.ok());
    ASSERT_EQ("1.2", json_str.value());
}

class JsonQueryTestFixture : public ::testing::TestWithParam<std::tuple<std::string, std::string, std::string>> {};

TEST_P(JsonQueryTestFixture, json_query) {
    std::unique_ptr<FunctionContext> ctx(FunctionContext::create_test_context());
    JsonColumn::Ptr ints = JsonColumn::create();
    ColumnBuilder<TYPE_VARCHAR> builder(1);

    std::string param_json = std::get<0>(GetParam());
    std::string param_path = std::get<1>(GetParam());
    std::string param_result = std::get<2>(GetParam());

    JsonValue json;
    ASSERT_TRUE(JsonValue::parse(param_json, &json).ok());
    ints->append(&json);
    if (param_path == "NULL") {
        builder.append_null();
    } else {
        builder.append(param_path);
    }

    Columns columns{ints, builder.build(true)};

    ctx.get()->set_constant_columns(columns);
    std::ignore =
            JsonFunctions::native_json_path_prepare(ctx.get(), FunctionContext::FunctionStateScope::FRAGMENT_LOCAL);

    ColumnPtr result = JsonFunctions::json_query(ctx.get(), columns).value();
    ASSERT_TRUE(!!result);

    StripWhiteSpace(&param_result);
    Datum datum = result->get(0);
    if (param_result == "NULL") {
        ASSERT_TRUE(datum.is_null());
    } else {
        ASSERT_TRUE(!datum.is_null());
        auto st = datum.get_json()->to_string();
        ASSERT_TRUE(st.ok()) << st->c_str();
        std::string json_result = datum.get_json()->to_string().value();
        StripWhiteSpace(&json_result);
        ASSERT_EQ(param_result, json_result);
    }

    ASSERT_TRUE(JsonFunctions::native_json_path_close(
                        ctx.get(), FunctionContext::FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                        .ok());
}

INSTANTIATE_TEST_SUITE_P(
        JsonQueryTest, JsonQueryTestFixture,
        ::testing::Values(
                // clang-format off
                // empty
                std::make_tuple(R"( {"k1":1} )", "NULL", R"(NULL)"),
                std::make_tuple(R"( {"k1":1} )", "$", R"( {"k1": 1} )"),
                std::make_tuple(R"( {"k1":1} )", "", R"(NULL)"),

                // various types
                std::make_tuple(R"( {"k1":1, "k2":"hehe", "k3":[1]} )", "$.k2", R"( "hehe" )"),
                std::make_tuple(R"( {"k1":1, "k2":"hehe", "k3":[1]} )", "$.k3", R"( [1] )"),
                std::make_tuple(R"( {"k1":1, "k2":"hehe", "k3":[1]} )", "$.k3", R"( [1] )"),
                std::make_tuple(R"( {"k1":1, "k2":"hehe", "k3":[1], "k4": {}} )", "$.k4", R"( {} )"),
                std::make_tuple(R"( {"k1":1, "k2":"hehe", "k3":[1], "k4": {}} )", "$.k5", R"( NULL )"),

                // simple syntax
                std::make_tuple(R"( {"k1":1, "k2":"hehe", "k3":[1]} )", "k2", R"( "hehe" )"),
                std::make_tuple(R"( {"k1":1, "k2":"hehe", "k3":[1]} )", "k3", R"( [1] )"),
                std::make_tuple(R"( {"k1":1, "k2":"hehe", "k3":[1]} )", "k3", R"( [1] )"),
                std::make_tuple(R"( {"k1":1, "k2":"hehe", "k3":[1], "k4": {}} )", "k4", R"( {} )"),
                std::make_tuple(R"( {"k1":1, "k2":"hehe", "k3":[1], "k4": {}} )", "k5", R"( NULL )"),

                // nested array
                std::make_tuple(R"( {"k1": [1,2,3]} )", "$.k1[0]", R"( 1 )"),
                std::make_tuple(R"( {"k1": [1,2,3]} )", "$.k1[3]", R"( NULL )"),
                std::make_tuple(R"( {"k1": [1,2,3]} )", "$.k1[-1]", R"( NULL )"),
                std::make_tuple(R"( {"k1": [[1,2,3], [4,5,6]]} )", "$.k1[0][0]", R"( 1 )"),
                std::make_tuple(R"( {"k1": [[1,2,3], [4,5,6]]} )", "$.k1[0][1]", R"( 2 )"),
                std::make_tuple(R"( {"k1": [[1,2,3], [4,5,6]]} )", "$.k1[0][2]", R"( 3 )"),
                std::make_tuple(R"( {"k1": [[1,2,3], [4,5,6]]} )", "$.k1[0][3]", R"( NULL )"),
                std::make_tuple(R"( {"k1": [[1,2,3], [4,5,6]]} )", "$.k1[1][0]", R"( 4 )"),
                std::make_tuple(R"( {"k1": [[1,2,3], [4,5,6]]} )", "$.k1[1][2]", R"( 6 )"),
                std::make_tuple(R"( {"k1": [[1,2,3], [4,5,6]]} )", "$.k1[2][0]", R"( NULL )"),
                std::make_tuple(R"( {"k1": [[1,2,3], [4,5,6]]} )", "$.k1[2]]]]]", R"( NULL )"),
                std::make_tuple(R"( {"k1": [[1,2,3], [4,5,6]]} )", "$.k1[[[[[2]", R"( NULL )"),
                std::make_tuple(R"( {"k1": [[[1,2,3]]]} )", "$.k1[0][0][0]", R"( 1 )"),
                std::make_tuple(R"( {"k1": [{"k2": [[1, 2], [3, 4]] }] } )", "$.k1[0].k2[0][0]", R"( 1 )"),
                std::make_tuple(R"( {"k1": [{"k2": [[1, 2], [3, 4]] }] } )", "$.k1[0].k2[1][0]", R"( 3 )"),

                // nested object
                std::make_tuple(R"( {"k1": {"k2": {"k3": 1}}} )", "$.k1", R"( {"k2": {"k3": 1}} )"),
                std::make_tuple(R"( {"k1": {"k2": {"k3": 1}}} )", "$.k1.k2", R"( {"k3": 1} )"),
                std::make_tuple(R"( {"k1": {"k2": {"k3": 1}}} )", "$.k1.k2.k3", R"( 1 )"),
                std::make_tuple(R"( {"k1": {"k2": {"k3": 1}}} )", "$.k1.k2.k3.k4", R"( NULL )"),

                std::make_tuple(R"( {"k1": {"k2": {"k3": 1}}} )", "k1", R"( {"k2": {"k3": 1}} )"),
                std::make_tuple(R"( {"k1": {"k2": {"k3": 1}}} )", "k1.k2", R"( {"k3": 1} )"),
                std::make_tuple(R"( {"k1": {"k2": {"k3": 1}}} )", "k1.k2.k3", R"( 1 )"),
                std::make_tuple(R"( {"k1": {"k2": {"k3": 1}}} )", "k1.k2.k3.k4", R"( NULL )"),

                // nested object in array
                std::make_tuple(R"( {"k1": [{"k2": 1}]} )", "$.k1[0]", R"( {"k2": 1} )"),
                std::make_tuple(R"( {"k1": [{"k2": 1}]} )", "$.k1[0].k2", R"( 1 )"),
                std::make_tuple(R"( {"k1": [{"k2": 1}]} )", "$.k1[0].k3", R"( NULL )"),

                // Top Level Array
                std::make_tuple(R"( [1,2,3] )", "$[1]", R"( 2 )"), 
                std::make_tuple(R"( [1,2,3] )", "$[5]", R"( NULL )"),
                std::make_tuple(R"( [1,2,3] )", "[1]",  R"( 2 )"), 
                std::make_tuple(R"( [1,2,3] )", "[5]",  R"( NULL )"),
                std::make_tuple(R"( [1,2,3] )", "[*]",  R"( [1, 2, 3] )"),
                std::make_tuple(R"( [1,2,3] )", "[*].k1",  R"( [] )"),
                std::make_tuple(R"( [{"k1": 1}, {"k1": 2}] )", "$[0]",      R"( {"k1": 1} )"),
                std::make_tuple(R"( [{"k1": 1}, {"k1": 2}] )", "$[0].k1",   R"( 1 )"),
                std::make_tuple(R"( [{"k1": 1}, {"k1": 2}] )", "$[*].k1",   R"( [1, 2] )"),
                std::make_tuple(R"( [{"k1": 1}, {"k2": 2}] )", "$[*].k1",   R"( [1] )"),
                std::make_tuple(R"( [{"k1": 1}, {"k2": 2}] )", "$[*].k2",   R"( [2] )"),
                std::make_tuple(R"( [{"k1": 1}, {"k2": 2}] )", "$[*]",      R"( [{"k1": 1}, {"k2": 2}] )"),
                std::make_tuple(R"( [[[0, 1, 2]]] )", "$[0][0][0]",      R"( 0 )"),
                std::make_tuple(R"( [[[0, 1, 2]]] )", "$[0][0][1]",      R"( 1 )"),

                // array result
                std::make_tuple(R"( {"k1": [{"k2": 1}, {"k2": 2}]} )", "$.k1[*].k2", R"( [1, 2] )"),
                std::make_tuple(R"( {"k1": [{"k2": 1}, {"k2": 2}]} )", "$.k1[*]", R"( [{"k2": 1}, {"k2": 2}] )"),
                std::make_tuple(R"( {"k1": [{"k2": 1}, {"k2": 2}, {"k2": 3}]} )", "$.k1[0:2]",
                                R"( [{"k2": 1}, {"k2": 2}] )"),
                std::make_tuple(R"( {"k1": [1,2,3,4]} )", "$.k1[*]", R"( [1, 2, 3, 4] )"),
                std::make_tuple(R"( {"k1": [1,2,3,4]} )", "$.k1[1:3]", R"( [2, 3] )")
                // clang-format on
                ));

class FlatJsonQueryTestFixture
        : public ::testing::TestWithParam<std::tuple<std::string, std::vector<std::string>, std::string, std::string>> {
};

TEST_P(FlatJsonQueryTestFixture, json_query) {
    std::unique_ptr<FunctionContext> ctx(FunctionContext::create_test_context());
    JsonColumn::Ptr json_col = JsonColumn::create();
    ColumnBuilder<TYPE_VARCHAR> builder(1);

    std::string param_json = std::get<0>(GetParam());
    std::vector<std::string> param_flat_path = std::get<1>(GetParam());

    std::string param_path = std::get<2>(GetParam());
    std::string param_result = std::get<3>(GetParam());

    JsonValue json;
    ASSERT_TRUE(JsonValue::parse(param_json, &json).ok());
    json_col->append(&json);
    if (param_path == "NULL") {
        builder.append_null();
    } else {
        builder.append(param_path);
    }

    JsonColumn::Ptr flat_json = JsonColumn::create();
    auto flat_json_ptr = flat_json.get();
    std::vector<LogicalType> param_flat_type;
    for (auto _ : param_flat_path) {
        param_flat_type.emplace_back(LogicalType::TYPE_JSON);
    }
    JsonFlattener jf(param_flat_path, param_flat_type, false);
    jf.flatten(json_col.get());
    flat_json_ptr->set_flat_columns(param_flat_path, param_flat_type, jf.mutable_result());

    Columns columns{flat_json, builder.build(true)};

    ctx.get()->set_constant_columns(columns);
    std::ignore =
            JsonFunctions::native_json_path_prepare(ctx.get(), FunctionContext::FunctionStateScope::FRAGMENT_LOCAL);

    ColumnPtr result = JsonFunctions::json_query(ctx.get(), columns).value();
    ASSERT_TRUE(!!result);

    StripWhiteSpace(&param_result);
    Datum datum = result->get(0);
    if (param_result == "NULL") {
        ASSERT_TRUE(datum.is_null());
    } else {
        ASSERT_TRUE(!datum.is_null());
        auto st = datum.get_json()->to_string();
        ASSERT_TRUE(st.ok()) << st->c_str();
        std::string json_result = datum.get_json()->to_string().value();
        StripWhiteSpace(&json_result);
        ASSERT_EQ(param_result, json_result);
    }

    ASSERT_TRUE(JsonFunctions::native_json_path_close(
                        ctx.get(), FunctionContext::FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                        .ok());
}

INSTANTIATE_TEST_SUITE_P(
        FlatJsonQueryTest, FlatJsonQueryTestFixture,
        ::testing::Values(
                // clang-format off
                // empty
                std::make_tuple(R"( {"k1":1} )", std::vector<std::string>{"k1"},  "NULL", R"(NULL)"),

                // various types
                std::make_tuple(R"( {"k1":1, "k2":"hehe", "k3":[1]} )", std::vector<std::string>{"k1", "k2", "k3"}, "$.k2", R"( "hehe" )"),
                std::make_tuple(R"( {"k1":1, "k2":"hehe", "k3":[1]} )", std::vector<std::string>{"k1", "k2", "k3"}, "$.k3", R"( [1] )"),
                std::make_tuple(R"( {"k1":1, "k2":"hehe", "k3":[1]} )", std::vector<std::string>{"k1", "k2", "k3"}, "$.k3", R"( [1] )"),
                std::make_tuple(R"( {"k1":1, "k2":"hehe", "k3":[1], "k4": {}} )", std::vector<std::string>{"k1", "k2", "k4"}, "$.k4", R"( {} )"),
                std::make_tuple(R"( {"k1":1, "k2":"hehe", "k3":[1], "k4": {}} )", std::vector<std::string>{"k1", "k2", "k5"}, "$.k5", R"( NULL )"),

                // simple syntax
                std::make_tuple(R"( {"k1":1, "k2":"hehe", "k3":[1]} )", std::vector<std::string>{"k1", "k2", "k3"}, "k2", R"( "hehe" )"),
                std::make_tuple(R"( {"k1":1, "k2":"hehe", "k3":[1]} )", std::vector<std::string>{"k1", "k2", "k3"}, "k3", R"( [1] )"),
                std::make_tuple(R"( {"k1":1, "k2":"hehe", "k3":[1]} )", std::vector<std::string>{"k1", "k2", "k3"}, "k3", R"( [1] )"),
                std::make_tuple(R"( {"k1":1, "k2":"hehe", "k3":[1], "k4": {}} )", std::vector<std::string>{"k1", "k4", "k3"}, "k4", R"( {} )"),
                std::make_tuple(R"( {"k1":1, "k2":"hehe", "k3":[1], "k4": {}} )", std::vector<std::string>{"k1", "k5", "k3"}, "k5", R"( NULL )"),

                // nested array
                std::make_tuple(R"( {"k1": [1,2,3]} )", std::vector<std::string>{"k1"}, "$.k1[0]", R"( 1 )"),
                std::make_tuple(R"( {"k1": [1,2,3]} )", std::vector<std::string>{"k1"}, "$.k1[3]", R"( NULL )"),
                std::make_tuple(R"( {"k1": [[1,2,3], [4,5,6]]} )", std::vector<std::string>{"k1"}, "$.k1[0][0]", R"( 1 )"),
                std::make_tuple(R"( {"k1": [[1,2,3], [4,5,6]]} )", std::vector<std::string>{"k1"}, "$.k1[0][1]", R"( 2 )"),
                std::make_tuple(R"( {"k1": [[1,2,3], [4,5,6]]} )", std::vector<std::string>{"k1"}, "$.k1[0][2]", R"( 3 )"),
                std::make_tuple(R"( {"k1": [[1,2,3], [4,5,6]]} )", std::vector<std::string>{"k1"}, "$.k1[0][3]", R"( NULL )"),
                std::make_tuple(R"( {"k1": [[1,2,3], [4,5,6]]} )", std::vector<std::string>{"k1"}, "$.k1[1][0]", R"( 4 )"),
                std::make_tuple(R"( {"k1": [[1,2,3], [4,5,6]]} )", std::vector<std::string>{"k1"}, "$.k1[1][2]", R"( 6 )"),
                std::make_tuple(R"( {"k1": [[1,2,3], [4,5,6]]} )", std::vector<std::string>{"k1"}, "$.k1[2][0]", R"( NULL )"),
                std::make_tuple(R"( {"k1": [[[1,2,3]]]} )", std::vector<std::string>{"k1"}, "$.k1[0][0][0]", R"( 1 )"),
                std::make_tuple(R"( {"k1": [{"k2": [[1, 2], [3, 4]] }] } )", std::vector<std::string>{"k1"}, "$.k1[0].k2[0][0]", R"( 1 )"),
                std::make_tuple(R"( {"k1": [{"k2": [[1, 2], [3, 4]] }] } )", std::vector<std::string>{"k1"}, "$.k1[0].k2[1][0]", R"( 3 )"),

                // nested object
                std::make_tuple(R"( {"k1": {"k2": {"k3": 1}}} )", std::vector<std::string>{"k1"}, "$.k1", R"( {"k2": {"k3": 1}} )"),
                std::make_tuple(R"( {"k1": {"k2": {"k3": 1}}} )", std::vector<std::string>{"k1"}, "$.k1.k2", R"( {"k3": 1} )"),
                std::make_tuple(R"( {"k1": {"k2": {"k3": 1}}} )", std::vector<std::string>{"k1"}, "$.k1.k2.k3", R"( 1 )"),
                std::make_tuple(R"( {"k1": {"k2": {"k3": 1}}} )", std::vector<std::string>{"k1"}, "$.k1.k2.k3.k4", R"( NULL )"),

                std::make_tuple(R"( {"k1": {"k2": {"k3": 1}}} )", std::vector<std::string>{"k1"}, "k1", R"( {"k2": {"k3": 1}} )"),
                std::make_tuple(R"( {"k1": {"k2": {"k3": 1}}} )", std::vector<std::string>{"k1"}, "k1.k2", R"( {"k3": 1} )"),
                std::make_tuple(R"( {"k1": {"k2": {"k3": 1}}} )", std::vector<std::string>{"k1"}, "k1.k2.k3", R"( 1 )"),
                std::make_tuple(R"( {"k1": {"k2": {"k3": 1}}} )", std::vector<std::string>{"k1"}, "k1.k2.k3.k4", R"( NULL )"),

                // nested object in array
                std::make_tuple(R"( {"k1": [{"k2": 1}]} )", std::vector<std::string>{"k1"}, "$.k1[0]", R"( {"k2": 1} )"),
                std::make_tuple(R"( {"k1": [{"k2": 1}]} )", std::vector<std::string>{"k1"}, "$.k1[0].k2", R"( 1 )"),
                std::make_tuple(R"( {"k1": [{"k2": 1}]} )", std::vector<std::string>{"k1"}, "$.k1[0].k3", R"( NULL )"),

                // array result
                std::make_tuple(R"( {"k1": [{"k2": 1}, {"k2": 2}]} )", std::vector<std::string>{"k1"}, "$.k1[*].k2", R"( [1, 2] )"),
                std::make_tuple(R"( {"k1": [{"k2": 1}, {"k2": 2}]} )", std::vector<std::string>{"k1"}, "$.k1[*]", R"( [{"k2": 1}, {"k2": 2}] )"),
                std::make_tuple(R"( {"k1": [{"k2": 1}, {"k2": 2}, {"k2": 3}]} )", std::vector<std::string>{"k1"}, "$.k1[0:2]",
                                R"( [{"k2": 1}, {"k2": 2}] )"),
                std::make_tuple(R"( {"k1": [1,2,3,4]} )", std::vector<std::string>{"k1"}, "$.k1[*]", R"( [1, 2, 3, 4] )"),
                std::make_tuple(R"( {"k1": [1,2,3,4]} )", std::vector<std::string>{"k1"}, "$.k1[1:3]", R"( [2, 3] )")
                // clang-format on
                ));

// 新增测试用例：验证JSON解析复用次数
TEST_F(JsonFunctionsTest, get_json_string_reuse_parse) {
    // 模拟可统计解析次数的parser
    struct MockParser {
        int parse_count = 0;
        simdjson::error_code iterate(const char* data, size_t size, simdjson::ondemand::document& doc) {
            parse_count++;
            return _real_parser.iterate(data, size, doc);
        }
        simdjson::ondemand::parser _real_parser;
    } mock_parser;

    // 准备测试数据
    std::unique_ptr<FunctionContext> ctx(FunctionContext::create_test_context());
    BinaryColumn::Ptr json_col = BinaryColumn::create();
    BinaryColumn::Ptr path_col = BinaryColumn::create();
    std::string json_str = R"({"field1": 100, "field2": "value", "field3": [1,2,3], "field4": {"sub": "test"}})";
    std::vector<std::string> paths = {"$.field1", "$.field2", "$.field3", "$.field4"};

    // 填充5次重复调用数据
    for (int i = 0; i < 5; ++i) {
        json_col->append(json_str);
        path_col->append(paths[i % 4]);
    }
    Columns columns = {json_col, path_col};
    ctx->set_constant_columns(columns);

    // 注入mock parser（需通过测试接口暴露内部parser）
    auto& internal_parser = ctx->impl()->get_json_parser_for_test(); // 假设存在测试接口
    internal_parser = &mock_parser;

    // 测试配置开启：应仅解析1次
    config::enable_json_reuse_parse = true;
    JsonFunctions::native_json_path_prepare(ctx.get(), FunctionContext::FunctionStateScope::FRAGMENT_LOCAL);
    ColumnPtr result_enable = JsonFunctions::get_json_string(ctx.get(), columns).value();
    JsonFunctions::native_json_path_close(ctx.get(), FunctionContext::FunctionStateScope::FRAGMENT_LOCAL);
    ASSERT_EQ(mock_parser.parse_count, 1);

    // 测试配置关闭：应解析5次
    mock_parser.parse_count = 0;
    config::enable_json_reuse_parse = false;
    JsonFunctions::native_json_path_prepare(ctx.get(), FunctionContext::FunctionStateScope::FRAGMENT_LOCAL);
    ColumnPtr result_disable = JsonFunctions::get_json_string(ctx.get(), columns).value();
    JsonFunctions::native_json_path_close(ctx.get(), FunctionContext::FunctionStateScope::FRAGMENT_LOCAL);
    ASSERT_EQ(mock_parser.parse_count, 5);

    // 验证结果正确性
    auto v_enable = ColumnHelper::cast_to<TYPE_VARCHAR>(result_enable);
    ASSERT_EQ("100", v_enable->get_data()[0].to_string());
    ASSERT_EQ("\"value\"", v_enable->get_data()[1].to_string());
    ASSERT_EQ("[1,2,3]", v_enable->get_data()[2].to_string());
    ASSERT_EQ("{\"sub\": \"test\"}", v_enable->get_data()[3].to_string());
}

class JsonExistTestFixture : public ::testing::TestWithParam<std::tuple<std::string, std::string, bool>> {};

TEST_P(JsonExistTestFixture, json_exists) {
    std::unique_ptr<FunctionContext> ctx(FunctionContext::create_test_context());
    JsonColumn::Ptr ints = JsonColumn::create();
    ColumnBuilder<TYPE_VARCHAR> builder(1);

    std::string param_json = std::get<0>(GetParam());
    std::string param_result = std::get<1>(GetParam());
    bool param_exist = std::get<2>(GetParam());

    auto json = JsonValue::parse(param_json);
    ASSERT_TRUE(json.ok());
    ints->append(&*json);
    if (param_result == "NULL") {
        builder.append_null();
    } else {
        builder.append(param_result);
    }

    Columns columns{ints, builder.build(true)};

    ctx.get()->set_constant_columns(columns);
    Status st = JsonFunctions::native_json_path_prepare(ctx.get(), FunctionContext::FunctionStateScope::FRAGMENT_LOCAL);
    if (!st.ok()) {
        ASSERT_FALSE(param_exist);
        return;
    }

    ColumnPtr result = JsonFunctions::json_exists(ctx.get(), columns).value();
    ASSERT_TRUE(!!result);

    if (param_exist) {
        ASSERT_TRUE((bool)result->get(0).get_uint8());
    } else {
        ASSERT_TRUE(result->get(0).is_null() || !(bool)result->get(0).get_uint8());
    }

    ASSERT_TRUE(JsonFunctions::native_json_path_close(
                        ctx.get(), FunctionContext::FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                        .ok());
}

INSTANTIATE_TEST_SUITE_P(JsonExistTest, JsonExistTestFixture,
                         ::testing::Values(std::make_tuple(R"({ "k1":1, "k2":"2"})", "$.k1", true),
                                           std::make_tuple(R"({ "k1": [1,2,3]})", "$.k1", true),
                                           std::make_tuple(R"({ "k1": {}})", "$.k1", true),
                                           std::make_tuple(R"({ "k1": {}})", "$.k1", true),
                                           // nested object
                                           std::make_tuple(R"({"k1": {"k2": {"k3": 1}}})", "$.k1.k2.k3", true),
                                           std::make_tuple(R"({"k1": [1]})", "$.k1[0]", true),

                                           // object in array
                                           std::make_tuple(R"({"k1": [{"k2": 1}]})", "$.k1[0].k2", true),

                                           // not exists
                                           std::make_tuple(R"({ })", "$.k1", false),
                                           std::make_tuple(R"({"k1": 1})", "$.k2", false),
                                           std::make_tuple(R"({"k1": {"k2": {"k3": 1}}})", "$.k1.k2.k3.k4", false),
                                           std::make_tuple(R"({"k1": {"k2": {"k3": 1}}})", "$.k1.k2.k4", false),
                                           std::make_tuple(R"({"k1": [{"k2": 1}]})", "$.k1[0].k3", false),

                                           //  nested array
                                           std::make_tuple(R"({"k1": [[1]]})", "$.k1[0][1]", false),
                                           std::make_tuple(R"({"k1": [[1]]})", "$.k1[0][0]", true),

                                           // special case
                                           std::make_tuple(R"({ "k1": {}})", "$", true),
                                           std::make_tuple(R"({ "k1": {}})", "", false),
                                           std::make_tuple(R"( { "k1": 1} )", "NULL", false),

                                           // error case
                                           std::make_tuple(R"( {"k1": null} )", std::string(10, 0x1), false)));

class FlatJsonExistsTestFixture
        : public ::testing::TestWithParam<std::tuple<std::string, std::vector<std::string>, std::string, bool>> {};

TEST_P(FlatJsonExistsTestFixture, flat_json_exists_test) {
    std::unique_ptr<FunctionContext> ctx(FunctionContext::create_test_context());
    JsonColumn::Ptr json_col = JsonColumn::create();

    std::string param_json = std::get<0>(GetParam());
    std::vector<std::string> param_flat_path = std::get<1>(GetParam());
    std::string param_path = std::get<2>(GetParam());
    bool param_exist = std::get<3>(GetParam());

    auto json = JsonValue::parse(param_json);
    ASSERT_TRUE(json.ok());
    json_col->append(&*json);

    Columns flat_columns;

    JsonColumn::Ptr flat_json = JsonColumn::create();
    auto* flat_json_ptr = down_cast<JsonColumn*>(flat_json.get());

    std::vector<LogicalType> param_flat_type;
    for (auto _ : param_flat_path) {
        param_flat_type.emplace_back(LogicalType::TYPE_JSON);
    }
    JsonFlattener jf(param_flat_path, param_flat_type, false);
    jf.flatten(json_col.get());
    flat_json_ptr->set_flat_columns(param_flat_path, param_flat_type, jf.mutable_result());

    Columns columns;
    columns.push_back(flat_json);
    if (!param_path.empty()) {
        BinaryColumn::Ptr path_column = BinaryColumn::create();
        path_column->append(param_path);
        columns.push_back(path_column);
    }

    ctx.get()->set_constant_columns(columns);
    Status st = JsonFunctions::native_json_path_prepare(ctx.get(), FunctionContext::FunctionStateScope::FRAGMENT_LOCAL);
    if (!st.ok()) {
        ASSERT_FALSE(param_exist);
        return;
    }

    ASSIGN_OR_ABORT(ColumnPtr result, JsonFunctions::json_exists(ctx.get(), columns))
    ASSERT_TRUE(!!result);

    if (param_exist) {
        ASSERT_TRUE((bool)result->get(0).get_uint8());
    } else {
        ASSERT_TRUE(result->get(0).is_null() || !(bool)result->get(0).get_uint8());
    }

    ASSERT_TRUE(JsonFunctions::native_json_path_close(
                        ctx.get(), FunctionContext::FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                        .ok());
}

// clang-format off
INSTANTIATE_TEST_SUITE_P(FlatJsonExistsTest, FlatJsonExistsTestFixture,
                          ::testing::Values(std::make_tuple(R"({ "k1":1, "k2":"2"})", std::vector<std::string>{"k1", "k2"}, "$.k1", true),
                                            std::make_tuple(R"({ "k1": [1,2,3]})", std::vector<std::string>({"k1", "k2"}),"$.k1", true),
                                            std::make_tuple(R"({"k1": {"k2": {"k3": 1}}})", std::vector<std::string>({"k1"}), "$.k1.k2.k3", true),
                                            std::make_tuple(R"({"k1": [{"k2": 1}]})", std::vector<std::string>({"k1"}), "$.k1[0].k2", true),
                                            std::make_tuple(R"({"k1": [{"k2": 1}]})", std::vector<std::string>({"k1"}), "$.k1[*].k2", true),
                                            std::make_tuple(R"({"k1": [{"k2": 1}]})", std::vector<std::string>({"k1"}), "$.k1[0:2].k2", true),
                                            std::make_tuple(R"({ })", std::vector<std::string>({"k1"}), "$.k1", false),
                                            std::make_tuple(R"({"k1": 1})", std::vector<std::string>({"k2"}), "$.k2", false),
                                            std::make_tuple(R"({"k1": {"k2": {"k3": 1}}})", std::vector<std::string>({"k1"}), "$.k1.k2.k3.k4", false),
                                            std::make_tuple(R"({"k1": [{"k2": 1}]})", std::vector<std::string>({"k1"}), "$.k1[0].k3", false),
                                            //  nested array
                                            std::make_tuple(R"({"k1": [[1]]})",std::vector<std::string>({"k1"}), "$.k1[0][1]", false),
                                            std::make_tuple(R"({"k1": [[1]]})",std::vector<std::string>({"k1"}), "$.k1[0][0]", true),
                                            // special case
                                            std::make_tuple(R"([{"k1": 1}, {"k2": 2}])",std::vector<std::string>({"k1"}), "$.k1[1]", false),
                                            std::make_tuple(R"("k1")",std::vector<std::string>({"k1"}), "$.k1", false)
                        ));
// clang-format on

TEST_F(JsonFunctionsTest, flat_json_invalid_path_test) {
    std::unique_ptr<FunctionContext> ctx(FunctionContext::create_test_context());
    JsonColumn::Ptr json_col = JsonColumn::create();

    std::string param_json = R"({"k1":1, "k2":"2"})";
    std::vector<std::string> param_flat_path = std::vector<std::string>{"k1", "k2"};
    std::string param_path = "$.k3";

    auto json = JsonValue::parse(param_json);
    ASSERT_TRUE(json.ok());
    json_col->append(&*json);

    Columns flat_columns;

    JsonColumn::Ptr flat_json = JsonColumn::create();
    auto* flat_json_ptr = down_cast<JsonColumn*>(flat_json.get());

    std::vector<LogicalType> param_flat_type;
    for (auto _ : param_flat_path) {
        param_flat_type.emplace_back(LogicalType::TYPE_JSON);
    }
    JsonFlattener jf(param_flat_path, param_flat_type, false);
    jf.flatten(json_col.get());
    flat_json_ptr->set_flat_columns(param_flat_path, param_flat_type, jf.mutable_result());

    Columns columns;
    columns.push_back(flat_json);
    if (!param_path.empty()) {
        BinaryColumn::Ptr path_column = BinaryColumn::create();
        path_column->append(param_path);
        columns.push_back(ConstColumn::create(path_column));
    }

    ctx.get()->set_constant_columns(columns);
    Status st = JsonFunctions::native_json_path_prepare(ctx.get(), FunctionContext::FunctionStateScope::FRAGMENT_LOCAL);
    if (!st.ok()) {
        return;
    }

    config::enable_lazy_dynamic_flat_json = false;
    auto ret = JsonFunctions::json_exists(ctx.get(), columns);
    config::enable_lazy_dynamic_flat_json = true;
    ASSERT_TRUE(JsonFunctions::native_json_path_close(
                        ctx.get(), FunctionContext::FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                        .ok());

    ASSERT_FALSE(ret.ok());
}

TEST_F(JsonFunctionsTest, flat_json_invalid_constant_json_test) {
    std::unique_ptr<FunctionContext> ctx(FunctionContext::create_test_context());
    JsonColumn::Ptr json_col = JsonColumn::create();

    std::string param_json = R"({"k1":1, "k2":"2"})";
    std::vector<std::string> param_flat_path = std::vector<std::string>{"k1", "k2"};
    std::string param_path = "$.k3";

    auto json = JsonValue::parse(param_json);
    ASSERT_TRUE(json.ok());
    json_col->append(&*json);

    Columns flat_columns;

    JsonColumn::Ptr flat_json = JsonColumn::create();
    auto* flat_json_ptr = down_cast<JsonColumn*>(flat_json.get());

    std::vector<LogicalType> param_flat_type;
    for (auto _ : param_flat_path) {
        param_flat_type.emplace_back(LogicalType::TYPE_JSON);
    }
    JsonFlattener jf(param_flat_path, param_flat_type, false);
    jf.flatten(json_col.get());
    flat_json_ptr->set_flat_columns(param_flat_path, param_flat_type, jf.mutable_result());

    Columns columns;
    columns.push_back(ConstColumn::create(flat_json, 2));
    if (!param_path.empty()) {
        BinaryColumn::Ptr path_column = BinaryColumn::create();
        path_column->append(param_path);
        columns.push_back(ConstColumn::create(path_column, 2));
    }

    ctx.get()->set_constant_columns(columns);
    ASSERT_TRUE(JsonFunctions::native_json_path_prepare(ctx.get(), FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                        .ok());

    auto ret = JsonFunctions::json_exists(ctx.get(), columns);
    ASSERT_TRUE(JsonFunctions::native_json_path_close(
                        ctx.get(), FunctionContext::FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                        .ok());

    ASSERT_FALSE(ret.ok());
}

TEST_F(JsonFunctionsTest, flat_json_variable_path_test) {
    std::unique_ptr<FunctionContext> ctx(FunctionContext::create_test_context());
    JsonColumn::Ptr json_col = JsonColumn::create();

    std::string param_json = R"({"k1":1, "k2":"2"})";
    std::vector<std::string> param_flat_path = std::vector<std::string>{"k1", "k2"};
    std::string param_path = "$.k2";

    auto json = JsonValue::parse(param_json);
    ASSERT_TRUE(json.ok());
    json_col->append(&*json);

    Columns flat_columns;

    JsonColumn::Ptr flat_json = JsonColumn::create();
    auto* flat_json_ptr = down_cast<JsonColumn*>(flat_json.get());

    std::vector<LogicalType> param_flat_type;
    for (auto _ : param_flat_path) {
        param_flat_type.emplace_back(LogicalType::TYPE_JSON);
    }
    JsonFlattener jf(param_flat_path, param_flat_type, false);
    jf.flatten(json_col.get());
    flat_json_ptr->set_flat_columns(param_flat_path, param_flat_type, jf.mutable_result());

    Columns columns;
    columns.push_back(flat_json);
    flat_json->assign(10, 0);

    if (!param_path.empty()) {
        BinaryColumn::Ptr path_column = BinaryColumn::create();
        path_column->append(param_path);
        path_column->assign(10, 0);
        columns.push_back(path_column);
    }

    ASSERT_TRUE(JsonFunctions::native_json_path_prepare(ctx.get(), FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                        .ok());
    ASSIGN_OR_ABORT(ColumnPtr result, JsonFunctions::json_exists(ctx.get(), columns))
    ASSERT_TRUE((bool)result->get(0).get_uint8());

    ASSERT_TRUE(JsonFunctions::native_json_path_close(
                        ctx.get(), FunctionContext::FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                        .ok());
}

TEST_F(JsonFunctionsTest, flat_json_invalid_variable_path_test) {
    std::unique_ptr<FunctionContext> ctx(FunctionContext::create_test_context());
    JsonColumn::Ptr json_col = JsonColumn::create();

    std::string param_json = R"({"k1":1, "k2":"2"})";
    std::vector<std::string> param_flat_path = std::vector<std::string>{"k1", "k2"};
    std::string param_path = "$.k2";

    auto json = JsonValue::parse(param_json);
    ASSERT_TRUE(json.ok());
    json_col->append(&*json);

    Columns flat_columns;

    JsonColumn::Ptr flat_json = JsonColumn::create();
    auto* flat_json_ptr = down_cast<JsonColumn*>(flat_json.get());

    std::vector<LogicalType> param_flat_type;
    for (auto _ : param_flat_path) {
        param_flat_type.emplace_back(LogicalType::TYPE_JSON);
    }
    JsonFlattener jf(param_flat_path, param_flat_type, false);
    jf.flatten(json_col.get());
    flat_json_ptr->set_flat_columns(param_flat_path, param_flat_type, jf.mutable_result());

    Columns columns;
    columns.push_back(flat_json);
    flat_json->assign(2, 0);

    if (!param_path.empty()) {
        BinaryColumn::Ptr path_column = BinaryColumn::create();
        path_column->append(param_path);
        path_column->append("$.k3");
        columns.push_back(path_column);
    }

    ASSERT_TRUE(JsonFunctions::native_json_path_prepare(ctx.get(), FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                        .ok());

    auto ret = JsonFunctions::json_exists(ctx.get(), columns);

    ASSERT_TRUE(JsonFunctions::native_json_path_close(
                        ctx.get(), FunctionContext::FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                        .ok());

    ASSERT_FALSE(ret.ok());
}

TEST_F(JsonFunctionsTest, flat_json_invalid_null_path_test) {
    std::unique_ptr<FunctionContext> ctx(FunctionContext::create_test_context());
    JsonColumn::Ptr json_col = JsonColumn::create();

    std::string param_json = R"({"k1":1, "k2":"2"})";
    std::vector<std::string> param_flat_path = std::vector<std::string>{"k1", "k2"};
    std::string param_path = "$.k2";

    auto json = JsonValue::parse(param_json);
    ASSERT_TRUE(json.ok());
    json_col->append(&*json);

    Columns flat_columns;

    JsonColumn::Ptr flat_json = JsonColumn::create();
    auto* flat_json_ptr = down_cast<JsonColumn*>(flat_json.get());

    std::vector<LogicalType> param_flat_type;
    for (auto _ : param_flat_path) {
        param_flat_type.emplace_back(LogicalType::TYPE_JSON);
    }
    JsonFlattener jf(param_flat_path, param_flat_type, false);
    jf.flatten(json_col.get());
    flat_json_ptr->set_flat_columns(param_flat_path, param_flat_type, jf.mutable_result());

    Columns columns;
    columns.push_back(flat_json);
    flat_json->assign(2, 0);

    if (!param_path.empty()) {
        NullableColumn::Ptr path_column = NullableColumn::create(BinaryColumn::create(), NullColumn::create());
        path_column->append_nulls(2);
        columns.push_back(path_column);
    }

    ASSERT_TRUE(JsonFunctions::native_json_path_prepare(ctx.get(), FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                        .ok());

    auto ret = JsonFunctions::json_exists(ctx.get(), columns);

    ASSERT_TRUE(JsonFunctions::native_json_path_close(
                        ctx.get(), FunctionContext::FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                        .ok());

    ASSERT_FALSE(ret.ok());
}

TEST_F(JsonFunctionsTest, flat_json_constant_path_test) {
    std::unique_ptr<FunctionContext> ctx(FunctionContext::create_test_context());
    JsonColumn::Ptr json_col = JsonColumn::create();

    std::string param_json = R"({"k1":1, "k2":"2"})";
    std::vector<std::string> param_flat_path = std::vector<std::string>{"k1", "k2"};
    std::string param_path = "$.k2";

    auto json = JsonValue::parse(param_json);
    ASSERT_TRUE(json.ok());
    json_col->append(&*json);

    Columns flat_columns;

    JsonColumn::Ptr flat_json = JsonColumn::create();
    auto* flat_json_ptr = down_cast<JsonColumn*>(flat_json.get());

    std::vector<LogicalType> param_flat_type;
    for (auto _ : param_flat_path) {
        param_flat_type.emplace_back(LogicalType::TYPE_JSON);
    }
    JsonFlattener jf(param_flat_path, param_flat_type, false);
    jf.flatten(json_col.get());
    flat_json_ptr->set_flat_columns(param_flat_path, param_flat_type, jf.mutable_result());

    Columns columns;
    columns.push_back(flat_json);

    if (!param_path.empty()) {
        BinaryColumn::Ptr path_column = BinaryColumn::create();
        path_column->append(param_path);
        columns.push_back(ConstColumn::create(path_column, 1));
    }

    ASSERT_TRUE(JsonFunctions::native_json_path_prepare(ctx.get(), FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                        .ok());

    ASSIGN_OR_ABORT(ColumnPtr result, JsonFunctions::json_exists(ctx.get(), columns))
    ASSERT_TRUE((bool)result->get(0).get_uint8());

    ASSERT_TRUE(JsonFunctions::native_json_path_close(
                        ctx.get(), FunctionContext::FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                        .ok());
}

class JsonParseTestFixture : public ::testing::TestWithParam<std::tuple<std::string, bool, std::string>> {};

TEST_P(JsonParseTestFixture, json_parse) {
    auto [param_json, param_ok, expected] = GetParam();
    std::unique_ptr<FunctionContext> ctx(FunctionContext::create_test_context());

    BinaryColumn::Ptr ints = BinaryColumn::create();
    ints->append(param_json);
    Columns columns{ints};
    ctx.get()->set_constant_columns(columns);

    ColumnPtr result = JsonFunctions::parse_json(ctx.get(), columns).value();
    ASSERT_TRUE(!!result);

    Datum datum = result->get(0);
    if (param_ok) {
        ASSERT_FALSE(datum.is_null());
        const JsonValue* json_value = datum.get_json();
        ASSERT_TRUE(!!json_value);
        std::string json_str = json_value->to_string().value();
        StripWhiteSpace(&json_str);
        StripWhiteSpace(&param_json);
        if (!expected.empty()) {
            ASSERT_EQ(expected, param_json);
        } else {
            ASSERT_EQ(param_json, json_str);
        }
    } else {
        ASSERT_TRUE(datum.is_null());
    }
}

INSTANTIATE_TEST_SUITE_P(JsonParseTest, JsonParseTestFixture,
                         ::testing::Values(
                                 // clang-format off
                                 
                                 // Parse as json object/array
                                 std::make_tuple(R"( {"k1": 1} )", true, ""), 
                                 std::make_tuple(R"( [1, 2, 3] )", true, ""), 
                                 std::make_tuple(R"( [] )", true, ""),
                                 std::make_tuple(R"( "a" )", true, ""),
                                 std::make_tuple(R"( "1" )", true, ""),

                                 // Parse as string
                                 std::make_tuple(R"( 2.1 )", true, R"(2.1)"), 
                                 std::make_tuple(R"( 1 )", true, R"(1)"), 
                                 std::make_tuple(R"( 1e5 )", true, R"(1e5)"), 
                                 std::make_tuple(R"( a1 )", true, R"(a1)"), 
                                 std::make_tuple(R"( 1a )", true, R"(1a)"), 
                                 std::make_tuple(R"( 1+1 )", true, "1+1"),
                                 std::make_tuple(R"( 2.x )", true, "2.x"),
                                 std::make_tuple(R"( nul )", true, "nul"),

                                 // failure
                                 std::make_tuple(R"( {"k1": 1 )", false, ""),
                                 std::make_tuple(R"( [,,,,,,] )", false, ""),
                                 std::make_tuple(R"( [1, )", false, ""),
                                 std::make_tuple(R"( "1 )", false, "")

                                 // clang-format on
                                 ));

class JsonArrayTestFixture : public ::testing::TestWithParam<std::tuple<std::vector<std::string>, std::string>> {};

TEST_P(JsonArrayTestFixture, json_array) {
    std::unique_ptr<FunctionContext> ctx(FunctionContext::create_test_context());

    Columns columns;
    std::vector<std::string> param_json = std::get<0>(GetParam());
    std::string param_result = std::get<1>(GetParam());
    for (const auto& json_str : param_json) {
        JsonColumn::Ptr column = JsonColumn::create();
        auto json = JsonValue::parse(json_str);
        ASSERT_TRUE(json.ok());
        column->append(&json.value());
        columns.emplace_back(std::move(column));
    }

    ctx.get()->set_constant_columns(columns);

    ColumnPtr result;
    if (param_json.empty()) {
        result = JsonFunctions::json_array_empty(ctx.get(), columns).value();
    } else {
        result = JsonFunctions::json_array(ctx.get(), columns).value();
    }
    ASSERT_TRUE(!!result);

    Datum datum = result->get(0);
    ASSERT_FALSE(datum.is_null());
    std::string json_str = datum.get_json()->to_string().value();
    StripWhiteSpace(&json_str);
    StripWhiteSpace(&param_result);
    ASSERT_EQ(param_result, json_str);
}

INSTANTIATE_TEST_SUITE_P(
        JsonArrayTest, JsonArrayTestFixture,
        ::testing::Values(std::make_tuple(std::vector<std::string>{}, "[]"),
                          std::make_tuple(std::vector<std::string>{"1", "2"}, R"(["1", "2"])"),
                          std::make_tuple(std::vector<std::string>{"1", "\"1\""}, R"(["1", "1"])"),
                          std::make_tuple(std::vector<std::string>{"1", R"({"a":1})"}, R"(["1", {"a": 1}])"),
                          std::make_tuple(std::vector<std::string>{"null", R"(1)"}, R"(["null", "1"])"),
                          std::make_tuple(std::vector<std::string>{"null", R"(null)"}, R"(["null", "null"])"),
                          std::make_tuple(std::vector<std::string>{"1", "1", "1"}, R"(["1", "1", "1"])"),
                          std::make_tuple(std::vector<std::string>{"1.1", "1.2"}, R"(["1.1", "1.2"])")));

using JsonObjectTestParam = std::tuple<std::vector<std::string>, std::string>;

class JsonObjectTestFixture : public ::testing::TestWithParam<JsonObjectTestParam> {};

TEST_P(JsonObjectTestFixture, json_object) {
    std::unique_ptr<FunctionContext> ctx(FunctionContext::create_test_context());

    Columns columns;
    std::vector<std::string> param_json = std::get<0>(GetParam());
    std::string param_result = std::get<1>(GetParam());
    for (const auto& json_str : param_json) {
        JsonColumn::Ptr column = JsonColumn::create();
        auto json = JsonValue::parse(json_str);
        ASSERT_TRUE(json.ok()) << "parse json failed: " << json_str;
        column->append(&json.value());
        columns.emplace_back(std::move(column));
    }

    ctx.get()->set_constant_columns(columns);

    ColumnPtr result;
    if (param_json.empty()) {
        result = JsonFunctions::json_object_empty(ctx.get(), columns).value();
    } else {
        result = JsonFunctions::json_object(ctx.get(), columns).value();
    }
    ASSERT_TRUE(!!result);

    Datum datum = result->get(0);
    if (param_result == "NULL") {
        ASSERT_TRUE(datum.is_null());
        return;
    }
    ASSERT_FALSE(datum.is_null());
    std::string json_str = datum.get_json()->to_string().value();
    StripWhiteSpace(&json_str);
    StripWhiteSpace(&param_result);
    ASSERT_EQ(param_result, json_str);
}

INSTANTIATE_TEST_SUITE_P(JsonObjectTest, JsonObjectTestFixture,
                         ::testing::Values(
                                 // clang-format off
                          JsonObjectTestParam(std::vector<std::string>{}, "{}"),
                          JsonObjectTestParam({R"("a")", "1", R"("b")", ""}, R"( {"a": "1", "b": ""} )"),
                          JsonObjectTestParam({R"("a")"}, R"({"a": null})"),
                          JsonObjectTestParam({R"("a")", R"("a")", R"("a")"}, R"({"a": "a", "a": null})"),
                          JsonObjectTestParam({R"("a")", R"("a")"}, R"({"a": "a"})"),
                          JsonObjectTestParam({R"("a")", "1"}, R"({"a": "1"})"),
                          JsonObjectTestParam({R"("a")", "1.234"}, R"({"a": "1.234"})"),
                          JsonObjectTestParam({R"("a")", "null"}, R"({"a": "null"})"),
                          JsonObjectTestParam({R"("a")", "true"}, R"({"a": "true"})"),
                          JsonObjectTestParam({R"("a")", "1", R"("b")", "2"}, R"({"a": "1", "b": "2"})"),
                          JsonObjectTestParam({R"("a")", "[1,2]"}, R"({"a": [1, 2]})"),
                          JsonObjectTestParam({R"("a")", R"({"b": 2})"}, R"({"a": {"b": 2}})"),

                          // illegal object
                          JsonObjectTestParam({"1", "1"}, R"({"1": "1"})"), 
                          JsonObjectTestParam({"1"}, R"({"1": null})"),
                          JsonObjectTestParam({R"("a")", "1", "1"}, R"({"1": null, "a": "1"})"),
                          JsonObjectTestParam({R"("")"}, R"(NULL)")

                                 // clang-format on
                                 ));

TEST_F(JsonFunctionsTest, extract_from_object_test) {
    std::string output;

    EXPECT_OK(test_extract_from_object(R"({"data" : 1})", "$.data", &output));
    EXPECT_STREQ(output.data(), "1");

    EXPECT_STATUS(Status::NotFound(""), test_extract_from_object(R"({"data" : 1})", "$.dataa", &output));

    EXPECT_OK(test_extract_from_object(R"({"data": [{"key": 1},{"key": 2}]})", "$.data[1].key", &output));
    EXPECT_STREQ(output.data(), "2");

    EXPECT_STATUS(Status::NotFound(""),
                  test_extract_from_object(R"({"data": [{"key": 1},{"key": 2}]})", "$.data[2].key", &output));

    EXPECT_STATUS(Status::NotFound(""),
                  test_extract_from_object(R"({"data": [{"key": 1},{"key": 2}]})", "$.data[3].key", &output));

    EXPECT_STATUS(Status::DataQualityError(""),
                  test_extract_from_object(R"({"data1 " : 1, "data2":})", "$.data", &output));

    EXPECT_STATUS(Status::NotFound(""), test_extract_from_object(R"({"data": null})", "$.data", &output));
    EXPECT_STATUS(Status::NotFound(""), test_extract_from_object(R"({"data": null})", "$.data.key", &output));

    EXPECT_OK(test_extract_from_object(R"({"data": {}})", "$.data", &output));
    EXPECT_STREQ(output.data(), "{}");
    EXPECT_STATUS(Status::NotFound(""), test_extract_from_object(R"({"data": {}})", "$.data.key", &output));

    EXPECT_STATUS(Status::NotFound(""), test_extract_from_object(R"({"data": 1})", "$.data.key", &output));

    EXPECT_OK(test_extract_from_object(R"({"key1": [1,2]})", "$.key1[1]", &output));
    EXPECT_STREQ(output.data(), "2");

    EXPECT_OK(test_extract_from_object(R"({"key1": [{"key2":3},{"key4": 5}]})", "$.key1[1].key4", &output));
    EXPECT_STREQ(output.data(), "5");

    EXPECT_STATUS(Status::NotFound(""), test_extract_from_object(R"({"key1": null})", "$.key1[1].key4", &output));
}

class JsonLengthTestFixture : public ::testing::TestWithParam<std::tuple<std::string, std::string, int>> {};

TEST_P(JsonLengthTestFixture, json_length_test) {
    std::unique_ptr<FunctionContext> ctx(FunctionContext::create_test_context());
    JsonColumn::Ptr json_column = JsonColumn::create();

    std::string param_json = std::get<0>(GetParam());
    std::string param_path = std::get<1>(GetParam());
    int expect_length = std::get<2>(GetParam());

    auto json = JsonValue::parse(param_json);
    ASSERT_TRUE(json.ok());
    json_column->append(&*json);

    Columns columns;
    columns.push_back(json_column);
    if (!param_path.empty()) {
        BinaryColumn::Ptr path_column = BinaryColumn::create();
        path_column->append(param_path);
        columns.push_back(path_column);
    }

    // ctx.get()->set_constant_columns(columns);
    Status st = JsonFunctions::native_json_path_prepare(ctx.get(), FunctionContext::FunctionStateScope::FRAGMENT_LOCAL);
    ASSERT_OK(st);

    ColumnPtr result = JsonFunctions::json_length(ctx.get(), columns).value();
    ASSERT_TRUE(!!result);
    EXPECT_EQ(expect_length, result->get(0).get_int32());

    ASSERT_TRUE(JsonFunctions::native_json_path_close(
                        ctx.get(), FunctionContext::FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                        .ok());
}

// clang-format off
INSTANTIATE_TEST_SUITE_P(JsonLengthTest, JsonLengthTestFixture,
                         ::testing::Values(
                            std::make_tuple(R"({ "k1":1, "k2": 2 })", "", 2), 
                            std::make_tuple(R"({ "k1":1, "k2": {} })", "$.k2", 0), 
                            std::make_tuple(R"({ "k1":1, "k2": [1,2] })", "$.k2", 2), 
                            std::make_tuple(R"({ "k1":1, "k2": [1,2] })", "$.k3", 0), 
                            std::make_tuple(R"({ })", "", 0),
                            std::make_tuple(R"( [] )", "", 0), 
                            std::make_tuple(R"( [1] )", "", 1),
                            std::make_tuple(R"( null )", "", 1), 
                            std::make_tuple(R"( 1 )", "", 1)
                        ));
// clang-format on

class FlatJsonLengthTestFixture
        : public ::testing::TestWithParam<std::tuple<std::string, std::vector<std::string>, std::string, int>> {};

TEST_P(FlatJsonLengthTestFixture, flat_json_length_test) {
    std::unique_ptr<FunctionContext> ctx(FunctionContext::create_test_context());
    JsonColumn::Ptr json_col = JsonColumn::create();

    std::string param_json = std::get<0>(GetParam());
    std::vector<std::string> param_flat_path = std::get<1>(GetParam());
    std::string param_path = std::get<2>(GetParam());
    int expect_length = std::get<3>(GetParam());

    auto json = JsonValue::parse(param_json);
    ASSERT_TRUE(json.ok());
    json_col->append(&*json);

    Columns flat_columns;

    JsonColumn::Ptr flat_json = JsonColumn::create();
    auto* flat_json_ptr = down_cast<JsonColumn*>(flat_json.get());

    std::vector<LogicalType> param_flat_type;
    for (auto _ : param_flat_path) {
        param_flat_type.emplace_back(LogicalType::TYPE_JSON);
    }
    JsonFlattener jf(param_flat_path, param_flat_type, false);
    jf.flatten(json_col.get());
    flat_json_ptr->set_flat_columns(param_flat_path, param_flat_type, jf.mutable_result());

    Columns columns;
    columns.push_back(flat_json);
    if (!param_path.empty()) {
        BinaryColumn::Ptr path_column = BinaryColumn::create();
        path_column->append(param_path);
        columns.push_back(path_column);
    }

    // ctx.get()->set_constant_columns(columns);
    Status st = JsonFunctions::native_json_path_prepare(ctx.get(), FunctionContext::FunctionStateScope::FRAGMENT_LOCAL);
    ASSERT_OK(st);

    ASSIGN_OR_ABORT(ColumnPtr result, JsonFunctions::json_length(ctx.get(), columns));
    ASSERT_TRUE(!!result);
    EXPECT_EQ(expect_length, result->get(0).get_int32());

    ASSERT_TRUE(JsonFunctions::native_json_path_close(
                        ctx.get(), FunctionContext::FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                        .ok());
}

// clang-format off
INSTANTIATE_TEST_SUITE_P(FlatJsonLengthTest, FlatJsonLengthTestFixture,
                         ::testing::Values(
                            std::make_tuple(R"({ "k1":1, "k2": {} })", std::vector<std::string>({"k1", "k2"}), "$.k2", 0), 
                            std::make_tuple(R"({ "k1":1, "k2": [1,2] })", std::vector<std::string>({"k1", "k2"}), "$.k2", 2), 
                            std::make_tuple(R"({ "k1":1, "k2": [1,2] })", std::vector<std::string>({"k1", "k2", "k3"}), "$.k3", 0),
                            std::make_tuple(R"({ "k1":1, "k2": {"xx": 1} })", std::vector<std::string>{"k1", "k2"}, "$.k1", 1)
                        ));
// clang-format on

class JsonKeysTestFixture : public ::testing::TestWithParam<std::tuple<std::string, std::string, std::string>> {};

TEST_P(JsonKeysTestFixture, json_keys) {
    std::unique_ptr<FunctionContext> ctx(FunctionContext::create_test_context());
    JsonColumn::Ptr json_column = JsonColumn::create();

    std::string param_json = std::get<0>(GetParam());
    std::string param_path = std::get<1>(GetParam());
    std::string param_result = std::get<2>(GetParam());

    auto json = JsonValue::parse(param_json);
    ASSERT_TRUE(json.ok());
    json_column->append(&*json);

    Columns columns;
    columns.push_back(json_column);
    if (!param_path.empty()) {
        BinaryColumn::Ptr path_column = BinaryColumn::create();
        path_column->append(param_path);
        columns.push_back(path_column);
    }

    Status st = JsonFunctions::native_json_path_prepare(ctx.get(), FunctionContext::FunctionStateScope::FRAGMENT_LOCAL);
    ASSERT_OK(st);

    ColumnPtr result = JsonFunctions::json_keys(ctx.get(), columns).value();
    ASSERT_TRUE(!!result);

    if (param_result == "NULL") {
        EXPECT_TRUE(result->is_null(0));
    } else {
        const JsonValue* keys = result->get(0).get_json();
        std::string keys_str = keys->to_string_uncheck();
        EXPECT_EQ(param_result, keys_str);
    }

    ASSERT_TRUE(JsonFunctions::native_json_path_close(
                        ctx.get(), FunctionContext::FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                        .ok());
}

// clang-format off
INSTANTIATE_TEST_SUITE_P(JsonKeysTest, JsonKeysTestFixture,
                         ::testing::Values(std::make_tuple(R"({ "k1": 1, "k2": 2 })", "", R"(["k1", "k2"])"),
                                           std::make_tuple(R"({ "k1": "v1" })",  "", R"(["k1"])"),
                                           std::make_tuple(R"({ "k1": {"k2": 1} })",  "", R"(["k1"])"),
                                           std::make_tuple(R"({ })",  "", R"([])"),
                                           std::make_tuple(R"( [] )",  "", "NULL"),
                                           std::make_tuple(R"( 1 )",  "", "NULL"),
                                           std::make_tuple(R"( "hehe")", "", "NULL"),
                                           std::make_tuple(R"({ "k1": "v1" })",  "$.k1", R"(NULL)"),
                                           std::make_tuple(R"({ "k1": "v1" })",  "$.k3", R"(NULL)"),
                                           std::make_tuple(R"({ "k1": {"k2": 1} })",  "$.k1", R"(["k2"])")
                         ));

// clang-format on

// 1: JSON Input
// 2. Path
// 3. get_json_int execpted result
// 4. get_json_string expected result
// 5. get_json_double expected result
using GetJsonXXXParam = std::tuple<std::string, std::string, int, std::string, double>;

class GetJsonXXXTestFixture : public ::testing::TestWithParam<GetJsonXXXParam> {
public:
    StatusOr<Columns> setup() {
        _ctx = std::unique_ptr<FunctionContext>(FunctionContext::create_test_context());
        JsonColumn::Ptr ints = JsonColumn::create();
        ColumnBuilder<TYPE_VARCHAR> builder(1);

        std::string param_json = std::get<0>(GetParam());
        std::string param_path = std::get<1>(GetParam());

        JsonValue json;
        Status st = JsonValue::parse(param_json, &json);
        if (!st.ok()) {
            return st;
        }
        ints->append(&json);
        if (param_path == "NULL") {
            builder.append_null();
        } else {
            builder.append(param_path);
        }
        Columns columns{ints, builder.build(true)};

        _ctx->set_constant_columns(columns);
        std::ignore = JsonFunctions::native_json_path_prepare(_ctx.get(),
                                                              FunctionContext::FunctionStateScope::FRAGMENT_LOCAL);
        return columns;
    }

    void tear_down() {
        ASSERT_TRUE(JsonFunctions::native_json_path_close(
                            _ctx.get(), FunctionContext::FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                            .ok());
    }

public:
    std::unique_ptr<FunctionContext> _ctx;
};

TEST_P(GetJsonXXXTestFixture, get_json_int) {
    auto maybe_columns = setup();
    ASSERT_TRUE(maybe_columns.ok());
    DeferOp defer([&]() { tear_down(); });
    Columns columns = std::move(maybe_columns.value());

    int expected = std::get<2>(GetParam());

    ColumnPtr result = JsonFunctions::get_native_json_int(_ctx.get(), columns).value();
    ASSERT_TRUE(!!result);

    ASSERT_EQ(1, result->size());
    Datum datum = result->get(0);
    if (expected == 0) {
        ASSERT_TRUE(datum.is_null());
    } else {
        ASSERT_TRUE(!datum.is_null());
        int64_t value = datum.get_int32();
        ASSERT_EQ(expected, value);
    }
}

TEST_P(GetJsonXXXTestFixture, get_json_string) {
    auto maybe_columns = setup();
    ASSERT_TRUE(maybe_columns.ok());
    DeferOp defer([&]() { tear_down(); });
    Columns columns = std::move(maybe_columns.value());

    std::string param_result = std::get<3>(GetParam());
    StripWhiteSpace(&param_result);

    ColumnPtr result = JsonFunctions::get_native_json_string(_ctx.get(), columns).value();
    ASSERT_TRUE(!!result);

    ASSERT_EQ(1, result->size());
    Datum datum = result->get(0);
    if (param_result == "NULL") {
        ASSERT_TRUE(datum.is_null());
    } else {
        ASSERT_TRUE(!datum.is_null());
        auto value = datum.get_slice();
        std::string value_str(value);
        ASSERT_EQ(param_result, value_str);
    }
}

TEST_P(GetJsonXXXTestFixture, get_json_double) {
    auto maybe_columns = setup();
    ASSERT_TRUE(maybe_columns.ok());
    DeferOp defer([&]() { tear_down(); });
    Columns columns = std::move(maybe_columns.value());

    double expected = std::get<4>(GetParam());

    ColumnPtr result = JsonFunctions::get_native_json_double(_ctx.get(), columns).value();
    ASSERT_TRUE(!!result);
    ASSERT_EQ(1, result->size());

    Datum datum = result->get(0);
    if (expected == 0) {
        ASSERT_TRUE(datum.is_null());
    } else {
        ASSERT_TRUE(!datum.is_null());
        double value = datum.get_double();
        ASSERT_EQ(expected, value);
    }
}

INSTANTIATE_TEST_SUITE_P(GetJsonXXXTest, GetJsonXXXTestFixture,
                         ::testing::Values(
                                 // clang-format: off

                                 std::make_tuple(R"( {"k1":1} )", "NULL", 0, "NULL", 0.0),
                                 std::make_tuple(R"( {"k1":1} )", "$", 0, R"( {"k1": 1} )", 0.0),
                                 std::make_tuple(R"( {"k1":1} )", "", 0, R"( NULL )", 0.0),
                                 std::make_tuple(R"( {"k0": null} )", "$.k1", 0, "NULL", 0.0),
                                 std::make_tuple(R"( {"k1": 1} )", "$.k1", 1, "1", 1.0),
                                 std::make_tuple(R"( {"k1": -1} )", "$.k1", -1, R"( -1 )", -1),
                                 std::make_tuple(R"( {"k1": 1.1} )", "$.k1", 1, R"( 1.1 )", 1.1),
                                 std::make_tuple(R"( {"k1": 3.14} )", "$.k1", 3, R"( 3.14 )", 3.14),
                                 std::make_tuple(R"( {"k1": null} )", "$.k1", 0, R"( NULL )", 0.0),
                                 std::make_tuple(R"( {"k1": "value" } )", "$.k1", 0, R"( value )", 0.0),
                                 std::make_tuple(R"( {"k1": {"k2": 1}} )", "$.k1", 0, R"( {"k2": 1} )", 0.0),
                                 std::make_tuple(R"( {"k1": [1,2,3] } )", "$.k1", 0, R"( [1, 2, 3] )", 0.0),

                                 // nested path
                                 std::make_tuple(R"( {"k1.k2": [1,2,3] } )", "$.\"k1.k2\"", 0, R"( [1, 2, 3] )", 0.0)

                                 // clang-format: on
                                 ));

TEST_F(JsonFunctionsTest, struct_to_json) {
    std::unique_ptr<FunctionContext> ctx(FunctionContext::create_test_context());
    // Struct{id: xxx, name: yyy}
    std::vector<std::string> names{"id", "name"};
    std::vector<LogicalType> types{TYPE_INT, TYPE_VARCHAR};

    // Build struct column
    Columns fields{NullableColumn::create(Int64Column::create(), NullColumn::create()),
                   NullableColumn::create(BinaryColumn::create(), NullColumn::create())};
    StructColumn::Ptr struct_column = StructColumn::create(fields, names);
    struct_column->append_datum(DatumStruct{int64_t(1), Slice("park")});
    struct_column->append_datum(DatumStruct{int64_t(2), Slice("menlo")});

    // Cast to JSON
    Columns input_columns{struct_column};
    auto maybe_res = JsonFunctions::to_json(ctx.get(), input_columns);
    ASSERT_TRUE(maybe_res.ok());
    ColumnPtr ptr = maybe_res.value();

    ASSERT_EQ(2, ptr->size());
    Datum json1 = ptr->get(0);
    ASSERT_FALSE(json1.is_null());
    ASSERT_EQ(R"({"id": 1, "name": "park"})", json1.get_json()->to_string_uncheck());

    Datum json2 = ptr->get(1);
    ASSERT_FALSE(json2.is_null());
    ASSERT_EQ(R"({"id": 2, "name": "menlo"})", json2.get_json()->to_string_uncheck());
}

TEST_F(JsonFunctionsTest, map_to_json) {
    std::unique_ptr<FunctionContext> ctx(FunctionContext::create_test_context());

    // Build MAP<int, string> column
    {
        NullableColumn::Ptr key_column = NullableColumn::create(Int64Column::create(), NullColumn::create());
        NullableColumn::Ptr val_column = NullableColumn::create(BinaryColumn::create(), NullColumn::create());
        MapColumn::Ptr struct_column = MapColumn::create(key_column, val_column, UInt32Column::create());

        DatumMap map1;
        map1[int64_t(1)] = Slice("menlo");
        map1[int64_t(2)] = Slice("park");
        struct_column->append_datum(map1);
        DatumMap map2;
        map2[int64_t(3)] = Slice("palo");
        map2[int64_t(4)] = Slice("alto");
        struct_column->append_datum(map2);

        // Call to_json
        Columns input_columns{struct_column};
        auto maybe_res = JsonFunctions::to_json(ctx.get(), input_columns);
        ASSERT_TRUE(maybe_res.ok());
        ColumnPtr ptr = maybe_res.value();

        ASSERT_EQ(2, ptr->size());
        Datum json1 = ptr->get(0);
        ASSERT_FALSE(json1.is_null());
        ASSERT_EQ(R"({"1": "menlo", "2": "park"})", json1.get_json()->to_string_uncheck());

        Datum json2 = ptr->get(1);
        ASSERT_FALSE(json2.is_null());
        ASSERT_EQ(R"({"3": "palo", "4": "alto"})", json2.get_json()->to_string_uncheck());
    }

    // Build MAP<string, int> column
    {
        NullableColumn::Ptr key_column = NullableColumn::create(BinaryColumn::create(), NullColumn::create());
        NullableColumn::Ptr val_column = NullableColumn::create(Int64Column::create(), NullColumn::create());
        MapColumn::Ptr struct_column = MapColumn::create(key_column, val_column, UInt32Column::create());

        DatumMap map1;
        map1[Slice("menlo")] = int64_t(1);
        map1[Slice("park")] = int64_t(2);
        struct_column->append_datum(map1);
        DatumMap map2;
        map2[Slice("palo")] = int64_t(3);
        map2[Slice("")] = int64_t(4);
        struct_column->append_datum(map2);

        // Call to_json
        Columns input_columns{struct_column};
        auto maybe_res = JsonFunctions::to_json(ctx.get(), input_columns);
        ASSERT_TRUE(maybe_res.ok());
        ColumnPtr ptr = maybe_res.value();

        ASSERT_EQ(2, ptr->size());
        Datum json1 = ptr->get(0);
        ASSERT_FALSE(json1.is_null());
        ASSERT_EQ(R"({"menlo": 1, "park": 2})", json1.get_json()->to_string_uncheck());

        Datum json2 = ptr->get(1);
        ASSERT_FALSE(json2.is_null());
        ASSERT_EQ(R"({"palo": 3})", json2.get_json()->to_string_uncheck());
    }
}

TEST_F(JsonFunctionsTest, query_json_obj) {
    std::unique_ptr<FunctionContext> ctx(FunctionContext::create_test_context());
    ColumnBuilder<TYPE_VARCHAR> builder(1);
    builder.append("has_active_email");

    std::vector<std::string> param_flat_path{"other"};
    std::vector<LogicalType> param_flat_type{LogicalType::TYPE_JSON};

    auto flat_json1 = JsonColumn::create();
    ASSIGN_OR_ABORT(auto jj1, JsonValue::parse(R"({"a1": 1, "b1": 2})"));
    down_cast<JsonColumn*>(flat_json1.get())->append(jj1);

    auto remain = JsonColumn::create();
    ASSIGN_OR_ABORT(auto jj2, JsonValue::parse(R"({"has_active_email": false, "c1": 3})"));
    down_cast<JsonColumn*>(remain.get())->append(jj2);

    Columns flat_js{flat_json1, remain};

    auto json_col = JsonColumn::create();
    json_col->set_flat_columns(param_flat_path, param_flat_type, flat_js);

    Columns columns{std::move(json_col), builder.build(true)};

    ctx.get()->set_constant_columns(columns);
    std::ignore =
            JsonFunctions::native_json_path_prepare(ctx.get(), FunctionContext::FunctionStateScope::FRAGMENT_LOCAL);

    ASSIGN_OR_ABORT(ColumnPtr result, JsonFunctions::get_native_json_bool(ctx.get(), columns));
    ASSERT_TRUE(JsonFunctions::native_json_path_close(
                        ctx.get(), FunctionContext::FunctionContext::FunctionStateScope::FRAGMENT_LOCAL)
                        .ok());

    ASSERT_EQ(result->debug_string(), "[0]");
}
} // namespace starrocks
