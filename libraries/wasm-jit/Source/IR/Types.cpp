#include "Types.h"

#include <map>
#include <memory>

namespace IR
{
	struct FunctionTypeMap
	{
		struct Key
		{
			ResultType ret;
			std::vector<ValueType> parameters;

			friend bool operator==(const Key& left,const Key& right) { return left.ret == right.ret && left.parameters == right.parameters; }
			friend bool operator!=(const Key& left,const Key& right) { return left.ret != right.ret || left.parameters != right.parameters; }
			friend bool operator<(const Key& left,const Key& right) { return left.ret < right.ret || (left.ret == right.ret && left.parameters < right.parameters); }
		};
		static std::map<Key,std::unique_ptr<FunctionType>>& get()
		{
			// Interned addresses remain stable for the cache lifetime. Own the
			// objects so normal shutdown (and failed insertion) cannot leak them.
			static std::map<Key,std::unique_ptr<FunctionType>> map;
			return map;
		}
	};

	template<typename Key,typename Value,typename CreateValueThunk>
	Value* findExistingOrCreateNew(std::map<Key,std::unique_ptr<Value>>& map,Key&& key,CreateValueThunk createValueThunk)
	{
		auto mapIt = map.find(key);
		if(mapIt != map.end()) { return mapIt->second.get(); }
		else
		{
			auto value = std::unique_ptr<Value>(createValueThunk());
			auto inserted = map.emplace(std::move(key),std::move(value));
			return inserted.first->second.get();
		}
	}

	const FunctionType* FunctionType::get(ResultType ret,const std::initializer_list<ValueType>& parameters)
	{ return findExistingOrCreateNew(FunctionTypeMap::get(),FunctionTypeMap::Key {ret,parameters},[=]{return new FunctionType(ret,parameters);}); }
	const FunctionType* FunctionType::get(ResultType ret,const std::vector<ValueType>& parameters)
	{ return findExistingOrCreateNew(FunctionTypeMap::get(),FunctionTypeMap::Key {ret,parameters},[=]{return new FunctionType(ret,parameters);}); }
	const FunctionType* FunctionType::get(ResultType ret)
	{ return findExistingOrCreateNew(FunctionTypeMap::get(),FunctionTypeMap::Key {ret,{}},[=]{return new FunctionType(ret,{});}); }
}
