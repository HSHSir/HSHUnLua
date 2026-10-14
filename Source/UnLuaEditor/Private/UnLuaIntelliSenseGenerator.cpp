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

#include "UnLuaIntelliSenseGenerator.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "CoreUObject.h"
#include "UnLua.h"
#include "UnLuaEditorSettings.h"
#include "UnLuaIntelliSense.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Image.h"
#include "Components/PanelWidget.h"
#include "Components/TextBlock.h"
#include "Blueprint/UserWidget.h"
#include "Engine/Blueprint.h"
#include "Interfaces/IPluginManager.h"
#include "Interfaces/IProjectManager.h"
#include "ProjectDescriptor.h"
#include "Dom/JsonObject.h"
#include "Misc/PackageName.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#define LOCTEXT_NAMESPACE "UnLuaIntelliSenseGenerator"

namespace
{
    constexpr int32 IntelliSenseManifestVersion = 2;

    FString GetCompactPropertyValue(const UObject* Object, const FName PropertyName)
    {
        if (!Object)
            return FString();

        const FProperty* Property = Object->GetClass()->FindPropertyByName(PropertyName);
        const UObject* DefaultObject = Object->GetClass()->GetDefaultObject();
        if (!Property || !DefaultObject)
            return FString();

        const void* Value = Property->ContainerPtrToValuePtr<void>(Object);
        const void* DefaultValue = Property->ContainerPtrToValuePtr<void>(DefaultObject);
        if (Property->Identical(Value, DefaultValue, PPF_None))
            return FString();

        FString ExportedValue;
        // UE5.8 split FProperty::ExportTextItem into _Direct / _InContainer
        // variants. The value pointer here is an in-container pointer, so the
        // _InContainer overload is the correct one.
        Property->ExportTextItem_InContainer(ExportedValue, Value, DefaultValue, const_cast<UObject*>(Object), PPF_None);
        ExportedValue.ReplaceInline(TEXT("\r"), TEXT(""));
        ExportedValue.ReplaceInline(TEXT("\n"), TEXT(" "));
        return ExportedValue;
    }

    void AppendProperty(FString& Description, const UObject* Object, const TCHAR* PropertyName)
    {
        const FString Value = GetCompactPropertyValue(Object, FName(PropertyName));
        if (!Value.IsEmpty())
            Description += FString::Printf(TEXT(" %s=%s"), PropertyName, *Value);
    }

    FString GetWidgetDescription(const UWidget* Widget)
    {
        FString Description;
        AppendProperty(Description, Widget, TEXT("Visibility"));
        AppendProperty(Description, Widget, TEXT("RenderOpacity"));
        AppendProperty(Description, Widget, TEXT("bIsEnabled"));

        if (const UTextBlock* TextBlock = Cast<UTextBlock>(Widget))
        {
            const FString Text = TextBlock->GetText().ToString().ReplaceCharWithEscapedChar();
            if (!Text.IsEmpty())
                Description += FString::Printf(TEXT(" Text=\"%s\""), *Text);

            // UE5.1+/5.2+ deprecate direct member access to Font/Brush in favour
            // of getters; use them so the editor-only IntelliSense path keeps
            // working on future engine versions.
            const FSlateFontInfo& FontInfo = TextBlock->GetFont();
            if (FontInfo.FontObject)
                Description += FString::Printf(TEXT(" Font=%s"), *FontInfo.FontObject->GetPathName());
            if (FontInfo.Size != 24)
                Description += FString::Printf(TEXT(" FontSize=%f"), FontInfo.Size);
        }
        else if (const UImage* Image = Cast<UImage>(Widget))
        {
            if (const UObject* ResourceObject = Image->GetBrush().GetResourceObject())
                Description += FString::Printf(TEXT(" Resource=%s"), *ResourceObject->GetPathName());
            AppendProperty(Description, Image, TEXT("ColorAndOpacity"));
        }
        else if (const UUserWidget* UserWidget = Cast<UUserWidget>(Widget))
        {
            Description += FString::Printf(TEXT(" WidgetClass=%s"), *UserWidget->GetClass()->GetName());
        }

        static const TCHAR* KeyProperties[] = {
            TEXT("WidthOverride"), TEXT("HeightOverride"), TEXT("MinDesiredWidth"), TEXT("MinDesiredHeight"),
            TEXT("MaxDesiredWidth"), TEXT("MaxDesiredHeight"), TEXT("Percent"), TEXT("BarFillType"),
            TEXT("Orientation"), TEXT("ContentPadding"), TEXT("ClickMethod"), TEXT("TouchMethod")
        };
        for (const TCHAR* PropertyName : KeyProperties)
            AppendProperty(Description, Widget, PropertyName);

        if (Widget->Slot)
        {
            FString SlotDescription;
            static const TCHAR* SlotProperties[] = {
                TEXT("LayoutData"), TEXT("Padding"), TEXT("HorizontalAlignment"), TEXT("VerticalAlignment"),
                TEXT("Size"), TEXT("ZOrder"), TEXT("bAutoSize"), TEXT("Row"), TEXT("Column"),
                TEXT("RowSpan"), TEXT("ColumnSpan"), TEXT("Layer"), TEXT("Nudge"), TEXT("FillSize"),
                TEXT("bFillEmptySpace"), TEXT("SizeRule")
            };
            for (const TCHAR* PropertyName : SlotProperties)
                AppendProperty(SlotDescription, Widget->Slot, PropertyName);

            if (!SlotDescription.IsEmpty())
                Description += FString::Printf(TEXT(" | %s:%s"), *Widget->Slot->GetClass()->GetName(), *SlotDescription.TrimStart());
        }
        return Description;
    }

    void AppendWidgetTree(const UWidget* Widget, const UClass* GeneratedClass, const FString& Prefix, const bool bIsLast, FString& Output)
    {
        if (!Widget)
            return;

        const bool bIsVariable = GeneratedClass && GeneratedClass->FindPropertyByName(Widget->GetFName()) != nullptr;
        const FString WidgetName = bIsVariable ? Widget->GetName() : FString::Printf(TEXT("[%s]"), *Widget->GetName());
        Output += FString::Printf(TEXT("%s%s%s (%s)%s\r\n"), *Prefix, bIsLast ? TEXT("└─ ") : TEXT("├─ "), *WidgetName,
            *Widget->GetClass()->GetName(), *GetWidgetDescription(Widget));

        const UPanelWidget* PanelWidget = Cast<UPanelWidget>(Widget);
        if (!PanelWidget)
            return;

        const FString ChildPrefix = Prefix + (bIsLast ? TEXT("   ") : TEXT("│  "));
        const int32 ChildCount = PanelWidget->GetChildrenCount();
        for (int32 Index = 0; Index < ChildCount; ++Index)
            AppendWidgetTree(PanelWidget->GetChildAt(Index), GeneratedClass, ChildPrefix, Index == ChildCount - 1, Output);
    }

    FString GetWidgetLayout(const UWidgetBlueprint* WidgetBlueprint)
    {
        if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree || !WidgetBlueprint->WidgetTree->RootWidget)
            return FString();

        FString Layout = TEXT("\r\n--[[ UserWidget Layout (auto-generated, do not edit)\r\n");
        AppendWidgetTree(WidgetBlueprint->WidgetTree->RootWidget, WidgetBlueprint->GeneratedClass, FString(), true, Layout);
        Layout += TEXT("]]\r\n");
        return Layout;
    }
}

TSharedPtr<FUnLuaIntelliSenseGenerator> FUnLuaIntelliSenseGenerator::Singleton;

TSharedRef<FUnLuaIntelliSenseGenerator> FUnLuaIntelliSenseGenerator::Get()
{
    if (!Singleton.IsValid())
        Singleton = MakeShareable(new FUnLuaIntelliSenseGenerator);
    return Singleton.ToSharedRef();
}

void FUnLuaIntelliSenseGenerator::Initialize()
{
    if (bInitialized)
        return;

    OutputDir = IPluginManager::Get().FindPlugin("UnLua")->GetBaseDir() + "/Intermediate/IntelliSense";
    ManifestPath = OutputDir / TEXT("IntelliSenseManifest.json");
    LoadManifest();

    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    AssetRegistryModule.Get().OnAssetAdded().AddRaw(this, &FUnLuaIntelliSenseGenerator::OnAssetAdded);
    AssetRegistryModule.Get().OnAssetRemoved().AddRaw(this, &FUnLuaIntelliSenseGenerator::OnAssetRemoved);
    AssetRegistryModule.Get().OnAssetRenamed().AddRaw(this, &FUnLuaIntelliSenseGenerator::OnAssetRenamed);
    AssetRegistryModule.Get().OnAssetUpdated().AddRaw(this, &FUnLuaIntelliSenseGenerator::OnAssetUpdated);

    bInitialized = true;
}

void FUnLuaIntelliSenseGenerator::UpdateAll(EUnLuaIntelliSenseScope Scope)
{
    const bool bGenerateWidgetLayout = GetDefault<UUnLuaEditorSettings>()->bGenerateWidgetLayout;
    if (bGenerateWidgetLayout != bCachedGenerateWidgetLayout)
    {
        AssetCache.Reset();
        bManifestDirty = true;
        bCachedGenerateWidgetLayout = bGenerateWidgetLayout;
    }

    const FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

    FARFilter Filter;
    // UE5.8 asserts on FARFilter::ClassNames (deprecated short names) when the
    // filter is compiled, and only ClassPaths are used. Set ClassPaths instead.
    Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
    Filter.ClassPaths.Add(UWidgetBlueprint::StaticClass()->GetClassPathName());

    if (Scope != EUnLuaIntelliSenseScope::All)
    {
        Filter.PackagePaths.Add(FName(TEXT("/Game")));
        Filter.bRecursivePaths = true;

        if (Scope == EUnLuaIntelliSenseScope::Project)
        {
            const TArray<TSharedRef<IPlugin>> EnabledPlugins = IPluginManager::Get().GetEnabledPlugins();
            for (const TSharedRef<IPlugin>& Plugin : EnabledPlugins)
            {
                if (Plugin->GetLoadedFrom() == EPluginLoadedFrom::Project && Plugin->CanContainContent())
                {
                    Filter.PackagePaths.Add(FName(*FString::Printf(TEXT("/%s"), *Plugin->GetName())));
                }
            }
        }
    }

    TArray<FAssetData> BlueprintAssets;
    TArray<const UField*> NativeTypes;
    TArray<const UField*> AllNativeTypes;
    AssetRegistryModule.Get().GetAssets(Filter, BlueprintAssets);
    CollectTypes(AllNativeTypes, EUnLuaIntelliSenseScope::All);

    if (Scope == EUnLuaIntelliSenseScope::All)
    {
        NativeTypes = AllNativeTypes;
    }
    else
    {
        TSet<FName> ProjectModules;
        TSet<FName> ProjectPluginModules;
        CollectProjectModules(ProjectModules, ProjectPluginModules);
        for (const UField* Type : AllNativeTypes)
        {
            if (IsNativeTypeInScope(Type, Scope, ProjectModules, ProjectPluginModules))
                NativeTypes.Add(Type);
        }
    }

    auto TotalCount = BlueprintAssets.Num() + NativeTypes.Num();
    if (TotalCount == 0)
    {
        ExportUE(AllNativeTypes);
        ExportUnLua();
        SaveManifest();
        return;
    }

    TotalCount++;

    FScopedSlowTask SlowTask(TotalCount, LOCTEXT("GeneratingBlueprintsIntelliSense", "Generating IntelliSense files"));
    SlowTask.MakeDialog(true);

    int32 GeneratedBlueprintCount = 0;
    int32 SkippedBlueprintCount = 0;
    bool bCancelled = false;
    for (int32 i = 0; i < BlueprintAssets.Num(); i++)
    {
        if (SlowTask.ShouldCancel())
        {
            bCancelled = true;
            break;
        }

        const FAssetData& AssetData = BlueprintAssets[i];
        if (ShouldRegenerateAsset(AssetData))
        {
            if (ShouldExport(AssetData, true))
            {
                UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetData.GetObjectPathString());
                if (Blueprint)
                {
                    const FString OutputFile = Export(Blueprint);
                    if (!OutputFile.IsEmpty())
                    {
                        UpdateAssetCache(AssetData, OutputFile);
                        GeneratedBlueprintCount++;
                    }
                }
            }
        }
        else
        {
            SkippedBlueprintCount++;
        }
        SlowTask.EnterProgressFrame();
    }

    if (!bCancelled)
    {
        for (const auto Type : NativeTypes)
        {
            if (SlowTask.ShouldCancel())
            {
                bCancelled = true;
                break;
            }

            Export(Type);
            SlowTask.EnterProgressFrame();
        }
    }

    if (bCancelled || SlowTask.ShouldCancel())
    {
        SaveManifest();
        return;
    }
    ExportUE(AllNativeTypes);
    ExportUnLua();
    SaveManifest();
    UE_LOG(LogUnLua, Log, TEXT("IntelliSense generation completed: %d Blueprints generated, %d unchanged Blueprints skipped, %d native types generated."), GeneratedBlueprintCount, SkippedBlueprintCount, NativeTypes.Num());
    SlowTask.EnterProgressFrame();
}

bool FUnLuaIntelliSenseGenerator::IsBlueprint(const FAssetData& AssetData)
{
    // UE5.8 only populates FAssetData::AssetClassPath (AssetClass is a deprecated
    // FName that is no longer written on construction), so compare path names.
    const FTopLevelAssetPath& AssetClassPath = AssetData.AssetClassPath;
    return AssetClassPath == UBlueprint::StaticClass()->GetClassPathName()
        || AssetClassPath == UWidgetBlueprint::StaticClass()->GetClassPathName();
}

bool FUnLuaIntelliSenseGenerator::ShouldExport(const FAssetData& AssetData, bool bLoad)
{
    const auto& Settings = *GetDefault<UUnLuaEditorSettings>();
    if (!Settings.bGenerateIntelliSense)
        return false;

    if (!IsBlueprint(AssetData))
        return false;

    const auto Asset = AssetData.FastGetAsset(bLoad);
    if (!Asset)
        return false;

    const auto Blueprint = Cast<UBlueprint>(Asset);
    if (!Blueprint)
        return false;

    if (Blueprint->SkeletonGeneratedClass || Blueprint->GeneratedClass)
        return true;

    return false;
}

FString FUnLuaIntelliSenseGenerator::Export(const UBlueprint* Blueprint)
{
    if (!Blueprint || !Blueprint->GeneratedClass)
        return FString();

    const UField* Field = Blueprint->GeneratedClass;
#if ENGINE_MAJOR_VERSION > 4 || (ENGINE_MAJOR_VERSION == 4 && ENGINE_MINOR_VERSION >= 26)
    const UPackage* Package = Field->GetPackage();
#else
    const UPackage* Package = (UPackage*)Field->GetTypedOuter(UPackage::StaticClass());
#endif
    if (!Package)
        return FString();

    FString ModuleName = Package->GetName();
    int32 LastSlashIndex;
    if (ModuleName.FindLastChar('/', LastSlashIndex))
        ModuleName.LeftInline(LastSlashIndex);

    FString FileName = UnLua::IntelliSense::GetTypeName(Field);
    if (FileName.EndsWith(TEXT("_C")))
        FileName.LeftChopInline(2);

    FString Content = UnLua::IntelliSense::Get(Field);
    const UUnLuaEditorSettings* Settings = GetDefault<UUnLuaEditorSettings>();
    if (Settings->bGenerateWidgetLayout)
    {
        if (const UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Blueprint))
            Content += GetWidgetLayout(WidgetBlueprint);
    }
    return SaveFile(ModuleName, FileName, Content);
}

FString FUnLuaIntelliSenseGenerator::GetOutputFile(const UField* Field) const
{
    if (!Field)
        return FString();

#if ENGINE_MAJOR_VERSION > 4 || (ENGINE_MAJOR_VERSION == 4 && ENGINE_MINOR_VERSION >= 26)
    const UPackage* Package = Field->GetPackage();
#else
    const UPackage* Package = (UPackage*)Field->GetTypedOuter(UPackage::StaticClass());
#endif
    if (!Package)
        return FString();

    FString ModuleName = Package->GetName();
    if (!Field->IsNative())
    {
        int32 LastSlashIndex;
        if (ModuleName.FindLastChar('/', LastSlashIndex))
            ModuleName.LeftInline(LastSlashIndex);
    }

    FString FileName = UnLua::IntelliSense::GetTypeName(Field);
    if (FileName.EndsWith("_C"))
        FileName.LeftChopInline(2);

    return FString::Printf(TEXT("%s/%s.lua"), *(OutputDir / ModuleName), *FileName);
}

FString FUnLuaIntelliSenseGenerator::Export(const UField* Field)
{
    const FString OutputFile = GetOutputFile(Field);
    if (OutputFile.IsEmpty())
        return FString();

#if ENGINE_MAJOR_VERSION > 4 || (ENGINE_MAJOR_VERSION == 4 && ENGINE_MINOR_VERSION >= 26)
    const UPackage* Package = Field->GetPackage();
#else
    const UPackage* Package = (UPackage*)Field->GetTypedOuter(UPackage::StaticClass());
#endif
    FString ModuleName = Package->GetName();
    if (!Field->IsNative())
    {
        int32 LastSlashIndex;
        if (ModuleName.FindLastChar('/', LastSlashIndex))
            ModuleName.LeftInline(LastSlashIndex);
    }

    FString FileName = UnLua::IntelliSense::GetTypeName(Field);
    if (FileName.EndsWith("_C"))
        FileName.LeftChopInline(2);
    const FString Content = UnLua::IntelliSense::Get(Field);
    return SaveFile(ModuleName, FileName, Content);
}

void FUnLuaIntelliSenseGenerator::ExportUE(const TArray<const UField*> Types)
{
    const FString Content = UnLua::IntelliSense::GetUE(Types);
    SaveFile("", "UE", Content);
}

void FUnLuaIntelliSenseGenerator::ExportUnLua()
{
    const auto ContentDir = IPluginManager::Get().FindPlugin(TEXT("UnLua"))->GetContentDir();
    const auto SrcDir = ContentDir / "IntelliSense";
    const auto DstDir = OutputDir;

    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    if (!PlatformFile.DirectoryExists(*SrcDir))
        return;

    PlatformFile.CopyDirectoryTree(*DstDir, *SrcDir, true);
}

void FUnLuaIntelliSenseGenerator::CollectProjectModules(TSet<FName>& ProjectModules, TSet<FName>& ProjectPluginModules)
{
    const FProjectDescriptor* Project = IProjectManager::Get().GetCurrentProject();
    if (Project)
    {
        for (const FModuleDescriptor& Module : Project->Modules)
        {
            ProjectModules.Add(Module.Name);
        }
    }

    const TArray<TSharedRef<IPlugin>> EnabledPlugins = IPluginManager::Get().GetEnabledPlugins();
    for (const TSharedRef<IPlugin>& Plugin : EnabledPlugins)
    {
        if (Plugin->GetLoadedFrom() != EPluginLoadedFrom::Project)
            continue;

        for (const FModuleDescriptor& Module : Plugin->GetDescriptor().Modules)
        {
            ProjectPluginModules.Add(Module.Name);
        }
    }
}

bool FUnLuaIntelliSenseGenerator::IsNativeTypeInScope(const UField* Field, EUnLuaIntelliSenseScope Scope, const TSet<FName>& ProjectModules, const TSet<FName>& ProjectPluginModules)
{
    if (Scope == EUnLuaIntelliSenseScope::All)
        return true;

    if (!Field || !Field->IsNative())
        return false;

#if ENGINE_MAJOR_VERSION > 4 || (ENGINE_MAJOR_VERSION == 4 && ENGINE_MINOR_VERSION >= 26)
    const UPackage* Package = Field->GetPackage();
#else
    const UPackage* Package = (UPackage*)Field->GetTypedOuter(UPackage::StaticClass());
#endif
    if (!Package)
        return false;

    const FName ModuleName(*FPackageName::GetShortName(Package->GetName()));
    if (ProjectModules.Contains(ModuleName))
        return true;

    return Scope == EUnLuaIntelliSenseScope::Project && ProjectPluginModules.Contains(ModuleName);
}

void FUnLuaIntelliSenseGenerator::CollectTypes(TArray<const UField*>& Types, EUnLuaIntelliSenseScope Scope)
{
    TSet<FName> ProjectModules;
    TSet<FName> ProjectPluginModules;
    if (Scope != EUnLuaIntelliSenseScope::All)
    {
        CollectProjectModules(ProjectModules, ProjectPluginModules);
    }

    for (TObjectIterator<UClass> It; It; ++It)
    {
        const UClass* Class = *It;
        const FString ClassName = Class->GetName();
        // ReSharper disable StringLiteralTypo
        if (ClassName.StartsWith("SKEL_")
            || ClassName.StartsWith("PLACEHOLDER-CLASS")
            || ClassName.StartsWith("REINST_")
            || ClassName.StartsWith("TRASHCLASS_")
            || ClassName.StartsWith("HOTRELOADED_")
        )
        {
            // skip nonsense types
            continue;
        }
        // ReSharper restore StringLiteralTypo
        if (IsNativeTypeInScope(Class, Scope, ProjectModules, ProjectPluginModules))
            Types.Add(Class);
    }

    for (TObjectIterator<UScriptStruct> It; It; ++It)
    {
        const UScriptStruct* ScriptStruct = *It;
        if (IsNativeTypeInScope(ScriptStruct, Scope, ProjectModules, ProjectPluginModules))
            Types.Add(ScriptStruct);
    }

    for (TObjectIterator<UEnum> It; It; ++It)
    {
        const UEnum* Enum = *It;
        if (IsNativeTypeInScope(Enum, Scope, ProjectModules, ProjectPluginModules))
            Types.Add(Enum);
    }
}

bool FUnLuaIntelliSenseGenerator::LoadManifest()
{
    AssetCache.Reset();
    FString JsonContent;
    if (!FFileHelper::LoadFileToString(JsonContent, *ManifestPath))
        return false;

    TSharedPtr<FJsonObject> RootObject;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonContent);
    if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
        return false;

    int32 Version = 0;
    if (!RootObject->TryGetNumberField(TEXT("Version"), Version) || Version != IntelliSenseManifestVersion)
        return false;

    bool bManifestGenerateWidgetLayout = false;
    if (!RootObject->TryGetBoolField(TEXT("GenerateWidgetLayout"), bManifestGenerateWidgetLayout)
        || bManifestGenerateWidgetLayout != GetDefault<UUnLuaEditorSettings>()->bGenerateWidgetLayout)
    {
        bManifestDirty = true;
        return false;
    }

    const TSharedPtr<FJsonObject>* AssetsObject = nullptr;
    if (!RootObject->TryGetObjectField(TEXT("Assets"), AssetsObject) || !AssetsObject || !AssetsObject->IsValid())
        return false;

    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*AssetsObject)->Values)
    {
        const TSharedPtr<FJsonObject> Entry = Pair.Value.IsValid() ? Pair.Value->AsObject() : nullptr;
        if (!Entry.IsValid())
            continue;

        FString PackageSizeString;
        FString PackageTimestampString;
        FString OutputFile;
        if (!Entry->TryGetStringField(TEXT("PackageSize"), PackageSizeString)
            || !Entry->TryGetStringField(TEXT("PackageTimestamp"), PackageTimestampString)
            || !Entry->TryGetStringField(TEXT("OutputFile"), OutputFile))
        {
            continue;
        }

        FUnLuaIntelliSenseAssetCache CacheEntry;
        CacheEntry.PackageSize = FCString::Atoi64(*PackageSizeString);
        CacheEntry.PackageTimestamp = FCString::Atoi64(*PackageTimestampString);
        CacheEntry.OutputFile = OutputFile;
        AssetCache.Add(Pair.Key, MoveTemp(CacheEntry));
    }
    return true;
}

bool FUnLuaIntelliSenseGenerator::SaveManifest()
{
    if (!bManifestDirty)
        return true;

    TSharedRef<FJsonObject> RootObject = MakeShareable(new FJsonObject);
    RootObject->SetNumberField(TEXT("Version"), IntelliSenseManifestVersion);
    RootObject->SetBoolField(TEXT("GenerateWidgetLayout"), GetDefault<UUnLuaEditorSettings>()->bGenerateWidgetLayout);

    TSharedRef<FJsonObject> AssetsObject = MakeShareable(new FJsonObject);
    for (const TPair<FString, FUnLuaIntelliSenseAssetCache>& Pair : AssetCache)
    {
        TSharedRef<FJsonObject> Entry = MakeShareable(new FJsonObject);
        Entry->SetStringField(TEXT("PackageSize"), FString::Printf(TEXT("%lld"), Pair.Value.PackageSize));
        Entry->SetStringField(TEXT("PackageTimestamp"), FString::Printf(TEXT("%lld"), Pair.Value.PackageTimestamp));
        Entry->SetStringField(TEXT("OutputFile"), Pair.Value.OutputFile);
        AssetsObject->SetObjectField(Pair.Key, Entry);
    }
    RootObject->SetObjectField(TEXT("Assets"), AssetsObject);

    FString JsonContent;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonContent);
    if (!FJsonSerializer::Serialize(RootObject, Writer))
        return false;

    IFileManager::Get().MakeDirectory(*OutputDir, true);
    const FString TemporaryPath = ManifestPath + TEXT(".tmp");
    if (!FFileHelper::SaveStringToFile(JsonContent, *TemporaryPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
        return false;

    IFileManager& FileManager = IFileManager::Get();
    if (!FileManager.Move(*ManifestPath, *TemporaryPath, true, true, false, true))
    {
        FileManager.Delete(*TemporaryPath);
        return false;
    }

    bManifestDirty = false;
    return true;
}

bool FUnLuaIntelliSenseGenerator::GetPackageFileState(const FAssetData& AssetData, int64& PackageSize, int64& PackageTimestamp)
{
    FString PackageFilename;
    // UE5.8 changed this overload to (LongPackageName, FString* OutFilename,
    // bool bAllowTextFormats); the old (.., FPackagePath*, FString*) shape is gone.
    if (!FPackageName::DoesPackageExist(AssetData.PackageName.ToString(), &PackageFilename))
        return false;

    IFileManager& FileManager = IFileManager::Get();
    PackageSize = FileManager.FileSize(*PackageFilename);
    if (PackageSize < 0)
        return false;

    PackageTimestamp = FileManager.GetTimeStamp(*PackageFilename).GetTicks();
    return PackageTimestamp > 0;
}

bool FUnLuaIntelliSenseGenerator::ShouldRegenerateAsset(const FAssetData& AssetData) const
{
    const FString ObjectPath = AssetData.GetObjectPathString();
    const FUnLuaIntelliSenseAssetCache* CacheEntry = AssetCache.Find(ObjectPath);
    if (!CacheEntry || CacheEntry->OutputFile.IsEmpty() || !IFileManager::Get().FileExists(*CacheEntry->OutputFile))
        return true;

    if (UPackage* LoadedPackage = FindPackage(nullptr, *AssetData.PackageName.ToString()))
    {
        if (LoadedPackage->IsDirty())
            return true;
    }

    int64 PackageSize = -1;
    int64 PackageTimestamp = 0;
    if (!GetPackageFileState(AssetData, PackageSize, PackageTimestamp))
        return true;

    return CacheEntry->PackageSize != PackageSize || CacheEntry->PackageTimestamp != PackageTimestamp;
}

void FUnLuaIntelliSenseGenerator::UpdateAssetCache(const FAssetData& AssetData, const FString& OutputFile)
{
    int64 PackageSize = -1;
    int64 PackageTimestamp = 0;
    if (!GetPackageFileState(AssetData, PackageSize, PackageTimestamp))
        return;

    FUnLuaIntelliSenseAssetCache& CacheEntry = AssetCache.FindOrAdd(AssetData.GetObjectPathString());
    CacheEntry.PackageSize = PackageSize;
    CacheEntry.PackageTimestamp = PackageTimestamp;
    CacheEntry.OutputFile = OutputFile;
    bManifestDirty = true;
}

void FUnLuaIntelliSenseGenerator::RemoveAssetCache(const FString& ObjectPath)
{
    if (AssetCache.Remove(ObjectPath) > 0)
        bManifestDirty = true;
}

FString FUnLuaIntelliSenseGenerator::GetBlueprintOutputFile(const FString& ObjectPath) const
{
    FString PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);
    FString AssetName = FPackageName::ObjectPathToObjectName(ObjectPath);
    if (PackageName.IsEmpty() || AssetName.IsEmpty())
        return FString();

    int32 LastSlashIndex;
    if (PackageName.FindLastChar('/', LastSlashIndex))
        PackageName.LeftInline(LastSlashIndex);
    return FString::Printf(TEXT("%s/%s.lua"), *(OutputDir / PackageName), *AssetName);
}

FString FUnLuaIntelliSenseGenerator::SaveFile(const FString& ModuleName, const FString& FileName, const FString& GeneratedFileContent)
{
    IFileManager& FileManager = IFileManager::Get();
    const FString Directory = OutputDir / ModuleName;
    if (!FileManager.DirectoryExists(*Directory))
        FileManager.MakeDirectory(*Directory, true);

    const FString FilePath = FString::Printf(TEXT("%s/%s.lua"), *Directory, *FileName);
    FString FileContent;
    FFileHelper::LoadFileToString(FileContent, *FilePath);
    if (FileContent != GeneratedFileContent
        && !FFileHelper::SaveStringToFile(GeneratedFileContent, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        UE_LOG(LogUnLua, Warning, TEXT("Failed to save IntelliSense file: %s"), *FilePath);
        return FString();
    }
    return FilePath;
}

void FUnLuaIntelliSenseGenerator::DeleteOutputFile(const FString& OutputFile)
{
    if (!OutputFile.IsEmpty() && IFileManager::Get().FileExists(*OutputFile))
        IFileManager::Get().Delete(*OutputFile);
}

void FUnLuaIntelliSenseGenerator::OnAssetAdded(const FAssetData& AssetData)
{
    if (!ShouldExport(AssetData))
        return;

    OnAssetUpdated(AssetData);

    TArray<const UField*> Types;
    CollectTypes(Types);
    ExportUE(Types);
}

void FUnLuaIntelliSenseGenerator::OnAssetRemoved(const FAssetData& AssetData)
{
    if (!IsBlueprint(AssetData))
        return;

    const FString ObjectPath = AssetData.GetObjectPathString();
    if (const FUnLuaIntelliSenseAssetCache* CacheEntry = AssetCache.Find(ObjectPath))
        DeleteOutputFile(CacheEntry->OutputFile);
    else
        DeleteOutputFile(GetBlueprintOutputFile(ObjectPath));

    RemoveAssetCache(ObjectPath);
    SaveManifest();
}

void FUnLuaIntelliSenseGenerator::OnAssetRenamed(const FAssetData& AssetData, const FString& OldPath)
{
    if (!IsBlueprint(AssetData))
        return;

    if (const FUnLuaIntelliSenseAssetCache* CacheEntry = AssetCache.Find(OldPath))
        DeleteOutputFile(CacheEntry->OutputFile);
    else
        DeleteOutputFile(GetBlueprintOutputFile(OldPath));

    RemoveAssetCache(OldPath);
    OnAssetUpdated(AssetData);
    SaveManifest();
}

void FUnLuaIntelliSenseGenerator::OnAssetUpdated(const FAssetData& AssetData)
{
    if (!ShouldExport(AssetData, true))
        return;

    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetData.GetObjectPathString());
    if (!Blueprint)
        return;

    const FString OutputFile = Export(Blueprint);
    if (!OutputFile.IsEmpty())
    {
        UpdateAssetCache(AssetData, OutputFile);
        SaveManifest();
    }
}
