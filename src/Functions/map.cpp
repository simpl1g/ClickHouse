#include <Functions/IFunction.h>
#include <Functions/FunctionFactory.h>
#include <Functions/FunctionHelpers.h>
#include <DataTypes/DataTypeMap.h>
#include <DataTypes/DataTypeTuple.h>
#include <DataTypes/DataTypesNumber.h>
#include <DataTypes/DataTypeFixedString.h>
#include <Columns/ColumnMap.h>
#include <Columns/ColumnArray.h>
#include <Columns/ColumnString.h>
#include <Columns/ColumnsNumber.h>
#include <DataTypes/getLeastSupertype.h>
#include <Interpreters/castColumn.h>
#include <memory>

#include <Common/assert_cast.h>
#include <Common/typeid_cast.h>
#include "array/arrayIndex.h"
#include "Functions/like.h"
#include "Functions/FunctionsStringSearch.h"


namespace DB
{
namespace ErrorCodes
{
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
    extern const int ILLEGAL_TYPE_OF_ARGUMENT;
}

namespace
{

// map(x, y, ...) is a function that allows you to make key-value pair
class FunctionMap : public IFunction
{
public:
    static constexpr auto name = "map";

    static FunctionPtr create(ContextPtr)
    {
        return std::make_shared<FunctionMap>();
    }

    String getName() const override
    {
        return name;
    }

    bool isVariadic() const override
    {
        return true;
    }

    size_t getNumberOfArguments() const override
    {
        return 0;
    }

    bool isInjective(const ColumnsWithTypeAndName &) const override
    {
        return true;
    }

    bool isSuitableForShortCircuitArgumentsExecution(const DataTypesWithConstInfo & /*arguments*/) const override { return true; }

    bool useDefaultImplementationForNulls() const override { return false; }
    bool useDefaultImplementationForConstants() const override { return true; }

    DataTypePtr getReturnTypeImpl(const DataTypes & arguments) const override
    {
        if (arguments.size() % 2 != 0)
            throw Exception(ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH,
                "Function {} requires even number of arguments, but {} given", getName(), arguments.size());

        DataTypes keys, values;
        for (size_t i = 0; i < arguments.size(); i += 2)
        {
            keys.emplace_back(arguments[i]);
            values.emplace_back(arguments[i + 1]);
        }

        DataTypes tmp;
        tmp.emplace_back(getLeastSupertype(keys));
        tmp.emplace_back(getLeastSupertype(values));
        return std::make_shared<DataTypeMap>(tmp);
    }

    ColumnPtr executeImpl(const ColumnsWithTypeAndName & arguments, const DataTypePtr & result_type, size_t input_rows_count) const override
    {
        size_t num_elements = arguments.size();

        if (num_elements == 0)
            return result_type->createColumnConstWithDefaultValue(input_rows_count);

        const auto & result_type_map = static_cast<const DataTypeMap &>(*result_type);
        const DataTypePtr & key_type = result_type_map.getKeyType();
        const DataTypePtr & value_type = result_type_map.getValueType();

        Columns columns_holder(num_elements);
        ColumnRawPtrs column_ptrs(num_elements);

        for (size_t i = 0; i < num_elements; ++i)
        {
            const auto & arg = arguments[i];
            const auto to_type = i % 2 == 0 ? key_type : value_type;

            ColumnPtr preprocessed_column = castColumn(arg, to_type);
            preprocessed_column = preprocessed_column->convertToFullColumnIfConst();

            columns_holder[i] = std::move(preprocessed_column);
            column_ptrs[i] = columns_holder[i].get();
        }

        /// Create and fill the result map.

        MutableColumnPtr keys_data = key_type->createColumn();
        MutableColumnPtr values_data = value_type->createColumn();
        MutableColumnPtr offsets = DataTypeNumber<IColumn::Offset>().createColumn();

        size_t total_elements = input_rows_count * num_elements / 2;
        keys_data->reserve(total_elements);
        values_data->reserve(total_elements);
        offsets->reserve(input_rows_count);

        IColumn::Offset current_offset = 0;
        for (size_t i = 0; i < input_rows_count; ++i)
        {
            for (size_t j = 0; j < num_elements; j += 2)
            {
                keys_data->insertFrom(*column_ptrs[j], i);
                values_data->insertFrom(*column_ptrs[j + 1], i);
            }

            current_offset += num_elements / 2;
            offsets->insert(current_offset);
        }

        auto nested_column = ColumnArray::create(
            ColumnTuple::create(Columns{std::move(keys_data), std::move(values_data)}),
            std::move(offsets));

        return ColumnMap::create(nested_column);
    }
};


struct NameMapContains { static constexpr auto name = "mapContains"; };

class FunctionMapContains : public IFunction
{
public:
    static constexpr auto name = NameMapContains::name;
    static FunctionPtr create(ContextPtr) { return std::make_shared<FunctionMapContains>(); }

    String getName() const override
    {
        return NameMapContains::name;
    }

    size_t getNumberOfArguments() const override { return impl.getNumberOfArguments(); }

    bool isSuitableForShortCircuitArgumentsExecution(const DataTypesWithConstInfo & arguments) const override
    {
        return impl.isSuitableForShortCircuitArgumentsExecution(arguments);
    }

    DataTypePtr getReturnTypeImpl(const ColumnsWithTypeAndName & arguments) const override
    {
        return impl.getReturnTypeImpl(arguments);
    }

    ColumnPtr executeImpl(const ColumnsWithTypeAndName & arguments, const DataTypePtr & result_type, size_t input_rows_count) const override
    {
        return impl.executeImpl(arguments, result_type, input_rows_count);
    }

private:
    FunctionArrayIndex<HasAction, NameMapContains> impl;
};


class FunctionMapKeys : public IFunction
{
public:
    static constexpr auto name = "mapKeys";
    static FunctionPtr create(ContextPtr) { return std::make_shared<FunctionMapKeys>(); }

    String getName() const override
    {
        return name;
    }

    size_t getNumberOfArguments() const override { return 1; }

    bool isSuitableForShortCircuitArgumentsExecution(const DataTypesWithConstInfo & /*arguments*/) const override { return true; }

    DataTypePtr getReturnTypeImpl(const ColumnsWithTypeAndName & arguments) const override
    {
        if (arguments.size() != 1)
            throw Exception("Number of arguments for function " + getName() + " doesn't match: passed "
                + toString(arguments.size()) + ", should be 1",
                ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);

        const DataTypeMap * map_type = checkAndGetDataType<DataTypeMap>(arguments[0].type.get());

        if (!map_type)
            throw Exception{"First argument for function " + getName() + " must be a map",
                ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT};

        auto key_type = map_type->getKeyType();

        return std::make_shared<DataTypeArray>(key_type);
    }

    bool useDefaultImplementationForConstants() const override { return true; }

    ColumnPtr executeImpl(const ColumnsWithTypeAndName & arguments, const DataTypePtr & /*result_type*/, size_t /*input_rows_count*/) const override
    {
        const ColumnMap * col_map = typeid_cast<const ColumnMap *>(arguments[0].column.get());
        if (!col_map)
            return nullptr;

        const auto & nested_column = col_map->getNestedColumn();
        const auto & keys_data = col_map->getNestedData().getColumn(0);

        return ColumnArray::create(keys_data.getPtr(), nested_column.getOffsetsPtr());
    }
};


class FunctionMapValues : public IFunction
{
public:
    static constexpr auto name = "mapValues";
    static FunctionPtr create(ContextPtr) { return std::make_shared<FunctionMapValues>(); }

    String getName() const override
    {
        return name;
    }

    size_t getNumberOfArguments() const override { return 1; }

    bool isSuitableForShortCircuitArgumentsExecution(const DataTypesWithConstInfo & /*arguments*/) const override { return true; }

    DataTypePtr getReturnTypeImpl(const ColumnsWithTypeAndName & arguments) const override
    {
        if (arguments.size() != 1)
            throw Exception("Number of arguments for function " + getName() + " doesn't match: passed "
                + toString(arguments.size()) + ", should be 1",
                ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);

        const DataTypeMap * map_type = checkAndGetDataType<DataTypeMap>(arguments[0].type.get());

        if (!map_type)
            throw Exception{"First argument for function " + getName() + " must be a map",
                ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT};

        auto value_type = map_type->getValueType();

        return std::make_shared<DataTypeArray>(value_type);
    }

    bool useDefaultImplementationForConstants() const override { return true; }

    ColumnPtr executeImpl(const ColumnsWithTypeAndName & arguments, const DataTypePtr & /*result_type*/, size_t /*input_rows_count*/) const override
    {
        const ColumnMap * col_map = typeid_cast<const ColumnMap *>(arguments[0].column.get());
        if (!col_map)
            return nullptr;

        const auto & nested_column = col_map->getNestedColumn();
        const auto & values_data = col_map->getNestedData().getColumn(1);

        return ColumnArray::create(values_data.getPtr(), nested_column.getOffsetsPtr());
    }
};

class FunctionMapContainsKeyLike : public IFunction
{
public:
    static constexpr auto name = "mapContainsKeyLike";
    static FunctionPtr create(ContextPtr) { return std::make_shared<FunctionMapContainsKeyLike>(); }
    String getName() const override { return name; }
    bool isSuitableForShortCircuitArgumentsExecution(const DataTypesWithConstInfo & /*info*/) const override { return true; }

    ColumnPtr executeImpl(const ColumnsWithTypeAndName & arguments, const DataTypePtr & result_type, size_t input_rows_count) const override
    {
        bool is_const = isColumnConst(*arguments[0].column);
        const ColumnMap * col_map = is_const ? checkAndGetColumnConstData<ColumnMap>(arguments[0].column.get())
                                             : checkAndGetColumn<ColumnMap>(arguments[0].column.get());
        const DataTypeMap * map_type = checkAndGetDataType<DataTypeMap>(arguments[0].type.get());
        if (!col_map || !map_type)
            throw Exception{"First argument for function " + getName() + " must be a map", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT};

        auto col_res = ColumnVector<UInt8>::create();
        typename ColumnVector<UInt8>::Container & vec_res = col_res->getData();

        if (input_rows_count == 0)
            return col_res;

        vec_res.resize(input_rows_count);

        const auto & column_array = typeid_cast<const ColumnArray &>(col_map->getNestedColumn());
        const auto & column_tuple = typeid_cast<const ColumnTuple &>(column_array.getData());

        const ColumnString * column_string = checkAndGetColumn<ColumnString>(column_tuple.getColumn(0));
        const ColumnFixedString * column_fixed_string = checkAndGetColumn<ColumnFixedString>(column_tuple.getColumn(0));

        FunctionLike func_like;

        for (size_t row = 0; row < input_rows_count; row++)
        {
            size_t element_start_row = row != 0 ? column_array.getOffsets()[row-1] : 0;
            size_t elem_size = column_array.getOffsets()[row]- element_start_row;

            ColumnPtr sub_map_column;
            DataTypePtr data_type;

            //The keys of one row map will be processed as a single ColumnString
            if (column_string)
            {
               sub_map_column = column_string->cut(element_start_row, elem_size);
               data_type = std::make_shared<DataTypeString>();
            }
            else
            {
               sub_map_column = column_fixed_string->cut(element_start_row, elem_size);
               data_type = std::make_shared<DataTypeFixedString>(checkAndGetColumn<ColumnFixedString>(sub_map_column.get())->getN());
            }

            size_t col_key_size = sub_map_column->size();
            auto column = is_const? ColumnConst::create(std::move(sub_map_column), std::move(col_key_size)) : std::move(sub_map_column);

            ColumnsWithTypeAndName new_arguments =
                {
                    {
                        column,
                        data_type,
                        ""
                    },
                    arguments[1]
                };

            auto res = func_like.executeImpl(new_arguments, result_type, input_rows_count);
            const auto & container = checkAndGetColumn<ColumnUInt8>(res.get())->getData();

            const auto it = std::find_if(container.begin(), container.end(), [](int element){ return element == 1; });  // NOLINT
            vec_res[row] = it == container.end() ? 0 : 1;
        }

        return col_res;
    }

    DataTypePtr getReturnTypeImpl(const ColumnsWithTypeAndName & arguments) const override
    {
        if (arguments.size() != 2)
            throw Exception("Number of arguments for function " + getName() + " doesn't match: passed "
                            + toString(arguments.size()) + ", should be 2",
                            ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);

        const DataTypeMap * map_type = checkAndGetDataType<DataTypeMap>(arguments[0].type.get());
        const DataTypeString * pattern_type = checkAndGetDataType<DataTypeString>(arguments[1].type.get());

        if (!map_type)
            throw Exception{"First argument for function " + getName() + " must be a Map",
                            ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT};
        if (!pattern_type)
            throw Exception{"Second argument for function " + getName() + " must be String",
                            ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT};

        if (!isStringOrFixedString(map_type->getKeyType()))
            throw Exception{"Key type of map for function " + getName() + " must be `String` or `FixedString`",
                            ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT};

        return std::make_shared<DataTypeUInt8>();
    }

    size_t getNumberOfArguments() const override { return 2; }

    bool useDefaultImplementationForConstants() const override { return true; }
};

}

void registerFunctionsMap(FunctionFactory & factory)
{
    factory.registerFunction<FunctionMap>();
    factory.registerFunction<FunctionMapContains>();
    factory.registerFunction<FunctionMapKeys>();
    factory.registerFunction<FunctionMapValues>();
    factory.registerFunction<FunctionMapContainsKeyLike>();
}

}
