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

using System;
using System.IO;
#if UE_5_0_OR_LATER
using EpicGames.Core;
#else
using Tools.DotNETCommon;
#endif
using UnrealBuildTool;

public class UnLua : ModuleRules
{
    public UnLua(ReadOnlyTargetRules Target) : base(Target)
    {
        // bEnforceIWYU was deprecated in UE5.2 in favour of IWYUSupport and is
        // gone from ModuleRules in UE5.8. IWYUSupport.None is the exact
        // equivalent of the previous bEnforceIWYU = false.
#if UE_5_2_OR_LATER
        IWYUSupport = IWYUSupport.None;
#else
        bEnforceIWYU = false;
#endif
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicIncludePaths.AddRange(
            new string[]
            {
            }
        );

        PrivateIncludePaths.AddRange(
            new[]
            {
                "UnLua/Private",
            }
        );

        PublicDependencyModuleNames.AddRange(new[]
        {
            "Lua"
        });

        PrivateDependencyModuleNames.AddRange(
            new[]
            {
                "Core",
                "CoreUObject",
                "Engine",
                "Slate",
                "InputCore"
            }
        );

        PublicIncludePaths.Add(Path.Combine(ModuleDirectory, "Private"));

        if (Target.bBuildEditor)
        {
            PrivateDependencyModuleNames.Add("UnrealEd");
            PrivateDependencyModuleNames.Add("DirectoryWatcher");
        }

        ConfigHierarchy config;
        const string section = "/Script/UnLuaEditor.UnLuaEditorSettings";
        
        // UE5.8: Use ConfigCache.ReadHierarchy instead of direct ConfigHierarchy construction
        if (Target.ProjectFile != null)
        {
            config = ConfigCache.ReadHierarchy(ConfigHierarchyType.Game, Target.ProjectFile.Directory, Target.Platform);
        }
        else
        {
            // Fallback for targets without project file (engine modules)
            config = new ConfigHierarchy(new ConfigFile[] { }, null);
        }

        Action<string, string, bool> loadBoolConfig = (key, macro, defaultValue) =>
        {
            bool flag;
            if (!config.GetBool(section, key, out flag))
                flag = defaultValue;
            PublicDefinitions.Add(string.Format("{0}={1}", macro, (flag ? "1" : "0")));
        };

        loadBoolConfig("bAutoStartup", "AUTO_UNLUA_STARTUP", true);
        loadBoolConfig("bEnableDebug", "UNLUA_ENABLE_DEBUG", false);
        loadBoolConfig("bEnablePersistentParamBuffer", "ENABLE_PERSISTENT_PARAM_BUFFER", true);
        loadBoolConfig("bEnableTypeChecking", "ENABLE_TYPE_CHECK", true);
        loadBoolConfig("bEnableRPCCall", "SUPPORTS_RPC_CALL", true);
        loadBoolConfig("bEnableCallOverriddenFunction", "ENABLE_CALL_OVERRIDDEN_FUNCTION", true);
        loadBoolConfig("bLuaCompileAsCpp", "LUA_COMPILE_AS_CPP", false);
        loadBoolConfig("bWithUE4Namespace", "WITH_UE4_NAMESPACE", true);
        loadBoolConfig("bLegacyReturnOrder", "UNLUA_LEGACY_RETURN_ORDER", false);
        loadBoolConfig("bLegacyBlueprintPath", "UNLUA_LEGACY_BLUEPRINT_PATH", false);
        loadBoolConfig("bLegacyAllowUTF8WithBOM", "UNLUA_LEGACY_ALLOW_BOM", false);
        loadBoolConfig("bEnableFText", "UNLUA_ENABLE_FTEXT", true);

        string hotReloadMode;
        if (!config.GetString(section, "HotReloadMode", out hotReloadMode))
            hotReloadMode = "Manual";

        var withHotReload = hotReloadMode != "Never";
        PublicDefinitions.Add("UNLUA_WITH_HOT_RELOAD=" + (withHotReload ? "1" : "0"));
    }
}