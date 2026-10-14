// Tencent is pleased to support the open source community by making UnLua available.
// 
// Copyright (C) 2019 THL A29 Limited, a Tencent company. All rights reserved.
//
// Licensed under the MIT License (the "License"); 
// you may not use this file except in compliance with the License. You may obtain a copy of the License at
//
// http://opensource.org/licenses/MIT
//
// Unless required by applicable law or agreed to in writing, 
// software distributed under the License is distributed on an "AS IS" BASIS, 
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. 
// See the License for the specific language governing permissions and limitations under the License.

#pragma once

#include "CoreUObject.h"
#include "Runtime/Launch/Resources/Version.h"
#include "Misc/EngineVersionComparison.h"

#if ENGINE_MAJOR_VERSION <= 4 && ENGINE_MINOR_VERSION < 19
#define DEFINE_FUNCTION(func) void func( FFrame& Stack, RESULT_DECL )
#define FNativeFuncPtr Native
#endif

#if ENGINE_MAJOR_VERSION <= 4 && ENGINE_MINOR_VERSION < 20
#define EPropertyFlags uint64
#define GET_INPUT_ACTION_NAME(IAB) IAB.ActionName
#define IS_INPUT_ACTION_PAIRED(IAB) IAB.bPaired
#else
#define GET_INPUT_ACTION_NAME(IAB) IAB.GetActionName()
#define IS_INPUT_ACTION_PAIRED(IAB) IAB.IsPaired()
#endif

#if ENGINE_MAJOR_VERSION <= 4 && ENGINE_MINOR_VERSION < 25
#define CastField Cast
#define GetPropertyOuter(Property) (Property)->GetOuter()
#define GetChildProperties(Function) (Function)->Children

typedef UProperty FProperty;
typedef UByteProperty FByteProperty;
typedef UInt8Property FInt8Property;
typedef UInt16Property FInt16Property;
typedef UIntProperty FIntProperty;
typedef UInt64Property FInt64Property;
typedef UUInt16Property FUInt16Property;
typedef UUInt32Property FUInt32Property;
typedef UUInt64Property FUInt64Property;
typedef UFloatProperty FFloatProperty;
typedef UDoubleProperty FDoubleProperty;
typedef UNumericProperty FNumericProperty;
typedef UEnumProperty FEnumProperty;
typedef UBoolProperty FBoolProperty;
typedef UObjectPropertyBase FObjectPropertyBase;
typedef UObjectProperty FObjectProperty;
typedef UClassProperty FClassProperty;
typedef UWeakObjectProperty FWeakObjectProperty;
typedef ULazyObjectProperty FLazyObjectProperty;
typedef USoftObjectProperty FSoftObjectProperty;
typedef USoftClassProperty FSoftClassProperty;
typedef UInterfaceProperty FInterfaceProperty;
typedef UNameProperty FNameProperty;
typedef UStrProperty FStrProperty;
typedef UTextProperty FTextProperty;
typedef UArrayProperty FArrayProperty;
typedef UMapProperty FMapProperty;
typedef USetProperty FSetProperty;
typedef UStructProperty FStructProperty;
typedef UDelegateProperty FDelegateProperty;
typedef UMulticastDelegateProperty FMulticastDelegateProperty;
#if ENGINE_MAJOR_VERSION > 4 || (ENGINE_MAJOR_VERSION == 4 && ENGINE_MINOR_VERSION > 22)
typedef UMulticastInlineDelegateProperty FMulticastInlineDelegateProperty;
typedef UMulticastSparseDelegateProperty FMulticastSparseDelegateProperty;
#endif
#else
#define GetPropertyOuter(Property) (Property)->GetOwnerUObject()
#define GetChildProperties(Function) (Function)->ChildProperties
#endif

#if ENGINE_MAJOR_VERSION < 5
typedef float unluaReal;
#else
typedef double unluaReal;
#endif

#if ENGINE_MAJOR_VERSION <= 4 && ENGINE_MINOR_VERSION < 23
typedef FMulticastScriptDelegate FMulticastDelegateType;
#else
typedef void FMulticastDelegateType;
#endif

template <typename T>
struct TMulticastDelegateTraits
{
    static const char* GetName() { return "FMulticastSparseDelegate"; }

    static void AddDelegate(FMulticastDelegateProperty* Property, FScriptDelegate Delegate, UObject* Parent, void* PropertyValue)
    {
#if ENGINE_MAJOR_VERSION > 4 || (ENGINE_MAJOR_VERSION == 4 && ENGINE_MINOR_VERSION > 22)
        Property->AddDelegate(Delegate, Parent, PropertyValue);
#endif
    }

    static void RemoveDelegate(FMulticastDelegateProperty* Property, FScriptDelegate Delegate, UObject* Parent, void* PropertyValue)
    {
#if ENGINE_MAJOR_VERSION > 4 || (ENGINE_MAJOR_VERSION == 4 && ENGINE_MINOR_VERSION > 22)
        Property->RemoveDelegate(Delegate, Parent, PropertyValue);
#endif
    }

    static void ClearDelegate(FMulticastDelegateProperty* Property, UObject* Parent, void* PropertyValue)
    {
#if ENGINE_MAJOR_VERSION > 4 || (ENGINE_MAJOR_VERSION == 4 && ENGINE_MINOR_VERSION > 22)
        Property->ClearDelegate(Parent, PropertyValue);
#endif
    }

    static FMulticastScriptDelegate* GetMulticastDelegate(FMulticastDelegateProperty* Property, void* PropertyValue)
    {
#if ENGINE_MAJOR_VERSION > 4 || (ENGINE_MAJOR_VERSION == 4 && ENGINE_MINOR_VERSION > 22)
        return (FMulticastScriptDelegate*)Property->GetMulticastDelegate(PropertyValue);
#else
        return nullptr;
#endif
    }
};

template <>
struct TMulticastDelegateTraits<FMulticastScriptDelegate>
{
    static const char* GetName()
    {
        return "FMulticastScriptDelegate";
    }

    static void AddDelegate(FMulticastDelegateProperty* Property, FScriptDelegate Delegate, UObject* Parent, void* PropertyValue)
    {
        ((FMulticastScriptDelegate*)PropertyValue)->AddUnique(Delegate);
    }
    
    static void RemoveDelegate(FMulticastDelegateProperty* Property, FScriptDelegate Delegate, UObject* Parent, void* PropertyValue)
    {
        ((FMulticastScriptDelegate*)PropertyValue)->Remove(Delegate);
    }
    
    static void ClearDelegate(FMulticastDelegateProperty* Property, UObject* Parent, void* PropertyValue)
    {
        ((FMulticastScriptDelegate*)PropertyValue)->Clear();
    }
    
    static FMulticastScriptDelegate* GetMulticastDelegate(FMulticastDelegateProperty* Property,  void* PropertyValue)
    {
        return (FMulticastScriptDelegate*)PropertyValue;
    }
};

// UE5.5+ ElementSize API adaptation
#if UE_VERSION_OLDER_THAN(5, 5, 0)
    #define UNLUA_GET_ELEMENT_SIZE(Property) ((Property)->ElementSize)
#else
    #define UNLUA_GET_ELEMENT_SIZE(Property) ((Property)->GetElementSize())
#endif

// UE5.7+ ANY_PACKAGE removal adaptation.
// ANY_PACKAGE is deprecated since 5.1 and fully removed in 5.7+.
// FindFirstObject performs a global search by name, which matches the old
// "search in any package" semantics. The engine exposes no
// FindFirstObjectChecked, so an asserting wrapper is provided here to preserve
// the original FindObjectChecked behaviour.
#if !UE_VERSION_OLDER_THAN(5, 7, 0)
namespace UnLua
{
    namespace Compat
    {
        template <typename T>
        T* FindFirstObjectChecked(FStringView Name)
        {
            T* Result = FindFirstObject<T>(Name);
            checkf(Result, TEXT("UnLua: object '%s' was not found"), *FString(Name));
            return Result;
        }
    }
}
#endif

#if UE_VERSION_OLDER_THAN(5, 7, 0)
    #define UNLUA_FIND_OBJECT(Type, Name) FindObject<Type>(ANY_PACKAGE, Name)
    #define UNLUA_FIND_OBJECT_CHECKED(Type, Name) FindObjectChecked<Type>(ANY_PACKAGE, Name)
#else
    #define UNLUA_FIND_OBJECT(Type, Name) FindFirstObject<Type>(Name)
    #define UNLUA_FIND_OBJECT_CHECKED(Type, Name) UnLua::Compat::FindFirstObjectChecked<Type>(Name)
#endif

// NOTE: the UE5.7+/5.8 removals of TChooseClass / TIsTriviallyDestructible are
// handled inside UnLuaTemplate.h (in namespace UnLua), and TIsSame / TRemoveCV
// were removed back in UE5.2 and are replaced by the standard library traits at
// their call sites. No global shims are needed here.
