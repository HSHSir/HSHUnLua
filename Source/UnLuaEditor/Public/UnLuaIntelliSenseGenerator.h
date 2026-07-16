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

class UBlueprint;
class UWidgetBlueprint;

struct FUnLuaIntelliSenseAssetCache
{
    int64 PackageSize = -1;
    int64 PackageTimestamp = 0;
    FString OutputFile;
};

enum class EUnLuaIntelliSenseScope : uint8
{
    All,
    Project,
    ProjectWithoutPlugins
};

class FUnLuaIntelliSenseGenerator
{
public:
    FUnLuaIntelliSenseGenerator()
        : bInitialized(false)
    {
    }

    static TSharedRef<FUnLuaIntelliSenseGenerator> Get();
    
    // Update IntelliSense files in the selected scope.
    void UpdateAll(EUnLuaIntelliSenseScope Scope = EUnLuaIntelliSenseScope::All);

    void Initialize();

private:
    static TSharedPtr<FUnLuaIntelliSenseGenerator> Singleton;

    static bool IsBlueprint(const FAssetData& AssetData);

    static bool ShouldExport(const FAssetData& AssetData, bool bLoad = false);

    FString Export(const UBlueprint* Blueprint);

    FString Export(const UField* Field);

    void ExportUE(const TArray<const UField*> Types);

    void ExportUnLua();

    void CollectTypes(TArray<const UField*>& Types, EUnLuaIntelliSenseScope Scope = EUnLuaIntelliSenseScope::All);

    static bool IsNativeTypeInScope(const UField* Field, EUnLuaIntelliSenseScope Scope, const TSet<FName>& ProjectModules, const TSet<FName>& ProjectPluginModules);

    static void CollectProjectModules(TSet<FName>& ProjectModules, TSet<FName>& ProjectPluginModules);

    bool LoadManifest();
    bool SaveManifest();
    bool ShouldRegenerateAsset(const FAssetData& AssetData) const;
    void UpdateAssetCache(const FAssetData& AssetData, const FString& OutputFile);
    void RemoveAssetCache(const FString& ObjectPath);
    static bool GetPackageFileState(const FAssetData& AssetData, int64& PackageSize, int64& PackageTimestamp);
    FString GetBlueprintOutputFile(const FString& ObjectPath) const;
    FString GetOutputFile(const UField* Field) const;

    // File helper
    FString SaveFile(const FString& ModuleName, const FString& FileName, const FString& GeneratedFileContent);
    void DeleteOutputFile(const FString& OutputFile);

    // Handle asset event
    void OnAssetAdded(const FAssetData& AssetData);
    void OnAssetRemoved(const FAssetData& AssetData);
    void OnAssetRenamed(const FAssetData& AssetData, const FString& OldPath);
    void OnAssetUpdated(const FAssetData& AssetData);

    FString OutputDir;
    FString ManifestPath;
    TMap<FString, FUnLuaIntelliSenseAssetCache> AssetCache;
    bool bManifestDirty = false;
    bool bCachedGenerateWidgetLayout = false;
    bool bInitialized;
};
