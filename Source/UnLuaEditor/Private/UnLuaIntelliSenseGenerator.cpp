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
#include "AssetRegistryModule.h"
#include "CoreUObject.h"
#include "UnLua.h"
#include "UnLuaEditorSettings.h"
#include "UnLuaIntelliSense.h"
#include "WidgetBlueprint.h"
#include "Animation/WidgetAnimation.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/Image.h"
#include "Components/PanelWidget.h"
#include "Components/TextBlock.h"
#include "Blueprint/UserWidget.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "K2Node_Event.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"
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
    constexpr int32 IntelliSenseManifestVersion = 4;

    // Exported struct text (brushes, styles, transforms) is clipped to keep the generated files readable.
    constexpr int32 MaxPropertyTextLength = 800;

    /**
     * Walks up the class hierarchy by name. This lets the generator describe widget and slot
     * families without a compile-time dependency on every UMG module header.
     */
    bool IsA(const UObject* Object, const TCHAR* ClassName)
    {
        const FName WantedClassName(ClassName);
        for (const UClass* Class = Object ? Object->GetClass() : nullptr; Class; Class = Class->GetSuperClass())
        {
            if (Class->GetFName() == WantedClassName)
                return true;
        }
        return false;
    }

    /** Compacts the default 6-digit float formatting ('0.000000' -> '0') so exported structs stay readable. */
    FString CompactNumbers(const FString& In)
    {
        FString Out;
        Out.Reserve(In.Len());

        int32 Index = 0;
        while (Index < In.Len())
        {
            const TCHAR Char = In[Index];
            if (Char == TEXT('.') && Index > 0 && FChar::IsDigit(In[Index - 1]))
            {
                int32 End = Index + 1;
                while (End < In.Len() && FChar::IsDigit(In[End]))
                    ++End;

                const FString Fraction = In.Mid(Index + 1, End - Index - 1);
                // Only touch real float formatting, never version-like strings such as '1.0.1'.
                if (Fraction.Len() >= 4)
                {
                    int32 Trimmed = Fraction.Len();
                    while (Trimmed > 0 && Fraction[Trimmed - 1] == TEXT('0'))
                        --Trimmed;
                    if (Trimmed > 0)
                    {
                        Out.AppendChar(TEXT('.'));
                        Out.AppendChars(*Fraction, Trimmed);
                    }
                    Index = End;
                    continue;
                }
            }

            Out.AppendChar(Char);
            ++Index;
        }
        return Out;
    }

    /** Keeps generated content on a single line and prevents it from terminating the Lua long comment. */
    FString EscapeForComment(const FString& In)
    {
        FString Out = In;
        Out.ReplaceInline(TEXT("\r\n"), TEXT("\\n"));
        Out.ReplaceInline(TEXT("\n"), TEXT("\\n"));
        Out.ReplaceInline(TEXT("\r"), TEXT("\\n"));
        Out.ReplaceInline(TEXT("]]"), TEXT("] ]"));
        return Out;
    }

    bool IsWidgetVariable(const UWidget* Widget, const UClass* GeneratedClass)
    {
        return Widget && GeneratedClass && GeneratedClass->FindPropertyByName(Widget->GetFName()) != nullptr;
    }

    /**
     * Expands a struct member by member through reflection. Native exporters such as FMargin or FAnchors
     * skip members that equal a zero-initialised default, which would turn '(Left=0,Top=0,...)' into '()'.
     */
    FString ExportStructFully(const UStruct* Struct, const void* Value, const int32 Depth = 0)
    {
        if (!Struct || !Value || Depth > 3)
            return FString();

        FString Out = TEXT("(");
        bool bAnyMember = false;
        for (TFieldIterator<FProperty> It(Struct, EFieldIteratorFlags::ExcludeSuper); It; ++It)
        {
            const FProperty* Property = *It;
            if (Property->ArrayDim > 1 || Property->HasAnyPropertyFlags(CPF_Deprecated))
                continue;

            const void* MemberValue = Property->ContainerPtrToValuePtr<void>(Value);
            FString MemberText;
            if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
                MemberText = ExportStructFully(StructProperty->Struct, MemberValue, Depth + 1);
            else
                Property->ExportTextItem(MemberText, MemberValue, nullptr, nullptr, PPF_None);

            if (MemberText.IsEmpty())
                continue;

            if (bAnyMember)
                Out += TEXT(",");
            Out += FString::Printf(TEXT("%s=%s"), *Property->GetName(), *MemberText);
            bAnyMember = true;
        }
        Out += TEXT(")");
        return Out;
    }

    FString GetPropertyText(const UObject* Object, const FName PropertyName, const bool bForce = false)
    {
        if (!Object)
            return FString();

        const FProperty* Property = Object->GetClass()->FindPropertyByName(PropertyName);
        const UObject* DefaultObject = Object->GetClass()->GetDefaultObject();
        if (!Property || !DefaultObject)
            return FString();

        const void* Value = Property->ContainerPtrToValuePtr<void>(Object);
        const void* DefaultValue = Property->ContainerPtrToValuePtr<void>(DefaultObject);
        if (!bForce && Property->Identical(Value, DefaultValue, PPF_None))
            return FString();

        FString ExportedValue;
        if (const FTextProperty* TextProperty = CastField<FTextProperty>(Property))
        {
            ExportedValue = FString::Printf(TEXT("\"%s\""), *TextProperty->GetPropertyValue(Value).ToString());
        }
        else if (bForce)
        {
            if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
                ExportedValue = ExportStructFully(StructProperty->Struct, Value);
            else
                Property->ExportTextItem(ExportedValue, Value, nullptr, const_cast<UObject*>(Object), PPF_None);
        }
        else
        {
            Property->ExportTextItem(ExportedValue, Value, DefaultValue, const_cast<UObject*>(Object), PPF_None);
        }

        ExportedValue = EscapeForComment(CompactNumbers(ExportedValue));
        if (ExportedValue.Len() > MaxPropertyTextLength)
        {
            ExportedValue.LeftInline(MaxPropertyTextLength);
            ExportedValue += TEXT("...<truncated>");
        }
        return ExportedValue;
    }

    void AppendProperties(FString& Out, const UObject* Object, const TArray<const TCHAR*>& PropertyNames, const bool bForce = false)
    {
        for (const TCHAR* PropertyName : PropertyNames)
        {
            const FString Value = GetPropertyText(Object, FName(PropertyName), bForce);
            if (!Value.IsEmpty())
                Out += FString::Printf(TEXT("%s=%s "), PropertyName, *Value);
        }
    }

    FString GetSlotDescription(const UPanelSlot* Slot)
    {
        if (!Slot)
            return TEXT("-");

        FString Properties;

        // Layout slots are reported even when they match the defaults: geometry is the whole point.
        if (IsA(Slot, TEXT("CanvasPanelSlot")))
        {
            AppendProperties(Properties, Slot, {
                TEXT("LayoutData"), TEXT("bAutoSize"), TEXT("ZOrder")
            }, true);
        }
        else if (IsA(Slot, TEXT("OverlaySlot")))
        {
            AppendProperties(Properties, Slot, {
                TEXT("Padding"), TEXT("HorizontalAlignment"), TEXT("VerticalAlignment")
            }, true);
        }
        else if (IsA(Slot, TEXT("VerticalBoxSlot")) || IsA(Slot, TEXT("HorizontalBoxSlot")))
        {
            AppendProperties(Properties, Slot, {
                TEXT("Size"), TEXT("SizeRule"), TEXT("SizeParam"), TEXT("Padding"),
                TEXT("HorizontalAlignment"), TEXT("VerticalAlignment")
            }, true);
        }
        else if (IsA(Slot, TEXT("GridSlot")) || IsA(Slot, TEXT("UniformGridSlot")))
        {
            AppendProperties(Properties, Slot, {
                TEXT("Row"), TEXT("Column"), TEXT("RowSpan"), TEXT("ColumnSpan"), TEXT("Layer"),
                TEXT("Nudge"), TEXT("Padding"), TEXT("HorizontalAlignment"), TEXT("VerticalAlignment")
            }, true);
        }
        else if (IsA(Slot, TEXT("WrapBoxSlot")))
        {
            AppendProperties(Properties, Slot, {
                TEXT("Padding"), TEXT("bFillEmptySpace"), TEXT("FillSpanWhenLessThan"),
                TEXT("HorizontalAlignment"), TEXT("VerticalAlignment")
            }, true);
        }
        else
        {
            AppendProperties(Properties, Slot, {
                TEXT("Padding"), TEXT("HorizontalAlignment"), TEXT("VerticalAlignment")
            }, true);
        }

        return FString::Printf(TEXT("%s{%s}"), *Slot->GetClass()->GetName(), *Properties.TrimEnd());
    }

    /** Core properties per widget family. Only values differing from the class default are emitted. */
    FString GetWidgetProperties(const UWidget* Widget)
    {
        FString Out;

        AppendProperties(Out, Widget, {
            TEXT("Visibility"), TEXT("RenderOpacity"), TEXT("bIsEnabled"), TEXT("ToolTipText"),
            TEXT("Clipping"), TEXT("RenderTransform"), TEXT("RenderTransformPivot")
        });

        if (IsA(Widget, TEXT("TextBlock")))
        {
            AppendProperties(Out, Widget, {
                TEXT("Text"), TEXT("ColorAndOpacity"), TEXT("Font"), TEXT("Justification"), TEXT("AutoWrapText"),
                TEXT("WrapTextAt"), TEXT("MinDesiredWidth"), TEXT("Margin"), TEXT("LineHeightPercentage"),
                TEXT("ShadowColorAndOpacity"), TEXT("ShadowOffset")
            });
        }

        if (IsA(Widget, TEXT("RichTextBlock")))
        {
            AppendProperties(Out, Widget, {
                TEXT("Text"), TEXT("TextStyleSet"), TEXT("AutoWrapText"), TEXT("WrapTextAt"),
                TEXT("Justification"), TEXT("Margin"), TEXT("MinDesiredWidth")
            });
        }

        if (IsA(Widget, TEXT("EditableText")) || IsA(Widget, TEXT("MultiLineEditableText")))
        {
            AppendProperties(Out, Widget, {
                TEXT("Text"), TEXT("HintText"), TEXT("IsReadOnly"), TEXT("IsPassword"), TEXT("Font"),
                TEXT("ColorAndOpacity"), TEXT("Justification"), TEXT("MinimumDesiredWidth"),
                TEXT("AutoWrapText"), TEXT("WrapTextAt"), TEXT("SelectAllTextWhenFocused")
            });
        }

        if (IsA(Widget, TEXT("Image")))
        {
            AppendProperties(Out, Widget, { TEXT("Brush"), TEXT("ColorAndOpacity") });
        }

        if (IsA(Widget, TEXT("Button")))
        {
            AppendProperties(Out, Widget, {
                TEXT("ColorAndOpacity"), TEXT("BackgroundColor"), TEXT("IsFocusable"),
                TEXT("ClickMethod"), TEXT("TouchMethod"), TEXT("PressMethod")
            });
        }

        if (IsA(Widget, TEXT("CheckBox")))
        {
            AppendProperties(Out, Widget, {
                TEXT("CheckedState"), TEXT("HorizontalAlignment"), TEXT("VerticalAlignment"),
                TEXT("ClickMethod"), TEXT("TouchMethod"), TEXT("PressMethod")
            });
        }

        if (IsA(Widget, TEXT("ProgressBar")))
        {
            AppendProperties(Out, Widget, {
                TEXT("Percent"), TEXT("BarFillType"), TEXT("BarFillStyle"), TEXT("IsMarquee"),
                TEXT("FillColorAndOpacity"), TEXT("BorderPadding")
            });
        }

        if (IsA(Widget, TEXT("Slider")))
        {
            AppendProperties(Out, Widget, {
                TEXT("Value"), TEXT("MinValue"), TEXT("MaxValue"), TEXT("StepSize"), TEXT("Orientation"),
                TEXT("SliderBarColor"), TEXT("SliderHandleColor")
            });
        }

        if (IsA(Widget, TEXT("ScrollBox")))
        {
            AppendProperties(Out, Widget, {
                TEXT("Orientation"), TEXT("ScrollBarVisibility"), TEXT("ConsumeMouseWheel"),
                TEXT("AlwaysShowScrollbar"), TEXT("AllowOverscroll"), TEXT("ScrollBarThickness")
            });
        }

        if (IsA(Widget, TEXT("ListView")) || IsA(Widget, TEXT("TileView")))
        {
            AppendProperties(Out, Widget, {
                TEXT("EntryWidgetClass"), TEXT("EntrySpacing"), TEXT("Orientation"),
                TEXT("ScrollBarVisibility"), TEXT("ConsumeMouseWheel")
            });
        }

        if (IsA(Widget, TEXT("SizeBox")))
        {
            AppendProperties(Out, Widget, {
                TEXT("WidthOverride"), TEXT("HeightOverride"), TEXT("MinDesiredWidth"), TEXT("MinDesiredHeight"),
                TEXT("MaxDesiredWidth"), TEXT("MaxDesiredHeight")
            });
        }

        if (IsA(Widget, TEXT("Border")))
        {
            AppendProperties(Out, Widget, {
                TEXT("BrushColor"), TEXT("Padding"), TEXT("HorizontalAlignment"), TEXT("VerticalAlignment"),
                TEXT("bShowEffectWhenDisabled")
            });
        }

        if (IsA(Widget, TEXT("WidgetSwitcher")))
        {
            AppendProperties(Out, Widget, { TEXT("ActiveWidgetIndex") });
        }

        if (IsA(Widget, TEXT("ComboBoxString")))
        {
            AppendProperties(Out, Widget, {
                TEXT("DefaultOptions"), TEXT("SelectedOption"), TEXT("Font"), TEXT("ForegroundColor"),
                TEXT("ContentPadding"), TEXT("MaxListHeight")
            });
        }

        if (IsA(Widget, TEXT("SpinBox")))
        {
            AppendProperties(Out, Widget, {
                TEXT("Value"), TEXT("MinValue"), TEXT("MaxValue"), TEXT("MinSliderValue"),
                TEXT("MaxSliderValue"), TEXT("Delta")
            });
        }

        if (IsA(Widget, TEXT("ScaleBox")))
        {
            AppendProperties(Out, Widget, {
                TEXT("Stretch"), TEXT("StretchDirection"), TEXT("UserSpecifiedScale"), TEXT("IgnoreInheritedScale")
            });
        }

        if (IsA(Widget, TEXT("Spacer")))
        {
            AppendProperties(Out, Widget, { TEXT("Size") });
        }

        if (IsA(Widget, TEXT("ExpandableArea")))
        {
            AppendProperties(Out, Widget, {
                TEXT("bIsExpanded"), TEXT("MaxHeight"), TEXT("AreaPadding"), TEXT("HeaderPadding")
            });
        }

        return Out;
    }

    /** Brush draw modes, printed by name instead of as a raw enum value. */
    const TCHAR* GetBrushDrawTypeName(const int32 DrawAs)
    {
        static const TCHAR* Names[] = { TEXT("NoDrawType"), TEXT("Box"), TEXT("Border"), TEXT("Image"), TEXT("RoundedBox") };
        return Names[DrawAs >= 0 && DrawAs < UE_ARRAY_COUNT(Names) ? DrawAs : 0];
    }

    /** Asset references the widget depends on (textures, fonts, material styles, nested widget classes). */
    FString GetWidgetAssetInfo(const UWidget* Widget)
    {
        if (const UImage* Image = Cast<UImage>(Widget))
        {
            if (const UObject* ResourceObject = Image->Brush.GetResourceObject())
                return FString::Printf(TEXT("brush=%s drawAs=%s"), *ResourceObject->GetPathName(),
                    GetBrushDrawTypeName(static_cast<int32>(Image->Brush.DrawAs)));
        }
        else if (const UButton* Button = Cast<UButton>(Widget))
        {
            FString Assets;
            const FSlateBrush* Brushes[] = {
                &Button->WidgetStyle.Normal, &Button->WidgetStyle.Hovered,
                &Button->WidgetStyle.Pressed, &Button->WidgetStyle.Disabled
            };
            const TCHAR* Roles[] = { TEXT("normal"), TEXT("hovered"), TEXT("pressed"), TEXT("disabled") };
            for (int32 Index = 0; Index < UE_ARRAY_COUNT(Brushes); ++Index)
            {
                if (const UObject* ResourceObject = Brushes[Index]->GetResourceObject())
                    Assets += FString::Printf(TEXT("%s=%s "), Roles[Index], *ResourceObject->GetPathName());
            }
            return Assets.TrimEnd();
        }
        else if (const UTextBlock* TextBlock = Cast<UTextBlock>(Widget))
        {
            if (TextBlock->Font.FontObject)
                return FString::Printf(TEXT("font=%s"), *TextBlock->Font.FontObject->GetPathName());
        }
        else if (const UUserWidget* UserWidget = Cast<UUserWidget>(Widget))
        {
            return FString::Printf(TEXT("widgetClass=%s"), *UserWidget->GetClass()->GetPathName());
        }

        return FString();
    }

    void AppendHierarchy(const UWidget* Widget, const UClass* GeneratedClass, const FString& Prefix, const bool bIsLast,
        FString& Output, int32& WidgetCount, int32& VariableCount)
    {
        if (!Widget)
            return;

        ++WidgetCount;
        const bool bIsVariable = IsWidgetVariable(Widget, GeneratedClass);
        if (bIsVariable)
            ++VariableCount;

        Output += FString::Printf(TEXT("%s%s%s (%s)%s\r\n"), *Prefix, bIsLast ? TEXT("└─ ") : TEXT("├─ "),
            *Widget->GetName(), *Widget->GetClass()->GetName(), bIsVariable ? TEXT(" [var]") : TEXT(" [no-var]"));

        const UPanelWidget* PanelWidget = Cast<UPanelWidget>(Widget);
        if (!PanelWidget)
            return;

        const FString ChildPrefix = Prefix + (bIsLast ? TEXT("   ") : TEXT("│  "));
        const int32 ChildCount = PanelWidget->GetChildrenCount();
        for (int32 Index = 0; Index < ChildCount; ++Index)
            AppendHierarchy(PanelWidget->GetChildAt(Index), GeneratedClass, ChildPrefix, Index == ChildCount - 1, Output, WidgetCount, VariableCount);
    }

    void AppendDetails(const UWidget* Widget, const UClass* GeneratedClass, const FString& ParentName, FString& Output)
    {
        if (!Widget)
            return;

        Output += FString::Printf(TEXT("  ## %s (%s)%s parent=%s\r\n"), *Widget->GetName(),
            *Widget->GetClass()->GetName(), IsWidgetVariable(Widget, GeneratedClass) ? TEXT(" [var]") : TEXT(" [no-var]"), *ParentName);
        Output += FString::Printf(TEXT("     slot  : %s\r\n"), *GetSlotDescription(Widget->Slot));

        const FString Properties = GetWidgetProperties(Widget);
        if (!Properties.IsEmpty())
            Output += FString::Printf(TEXT("     props : %s\r\n"), *Properties.TrimEnd());

        const FString Assets = GetWidgetAssetInfo(Widget);
        if (!Assets.IsEmpty())
            Output += FString::Printf(TEXT("     asset : %s\r\n"), *Assets);

        // Nested UserWidgets expose their own tree in their own IntelliSense file.
        const UPanelWidget* PanelWidget = Cast<UPanelWidget>(Widget);
        if (!PanelWidget)
            return;

        const int32 ChildCount = PanelWidget->GetChildrenCount();
        for (int32 Index = 0; Index < ChildCount; ++Index)
            AppendDetails(PanelWidget->GetChildAt(Index), GeneratedClass, Widget->GetName(), Output);
    }

    /** True when the property is a delegate, i.e. the binding is an event instead of a property binding. */
    bool IsDelegateProperty(const UObject* Object, const FName PropertyName)
    {
        if (!Object)
            return false;

        const FProperty* Property = Object->GetClass()->FindPropertyByName(PropertyName);
        return Property && (CastField<FMulticastDelegateProperty>(Property) != nullptr || CastField<FDelegateProperty>(Property) != nullptr);
    }

    /** UMG bindings: 'widget member . property -> blueprint function'. */
    FString GetWidgetBindings(const UWidgetBlueprint* WidgetBlueprint)
    {
        FString Out;
        for (const FDelegateEditorBinding& Binding : WidgetBlueprint->Bindings)
        {
            if (Binding.ObjectName.IsEmpty() || Binding.PropertyName.IsNone() || Binding.FunctionName.IsNone())
                continue;

            const UWidget* Widget = WidgetBlueprint->WidgetTree
                ? WidgetBlueprint->WidgetTree->FindWidget(FName(*Binding.ObjectName))
                : nullptr;

            Out += FString::Printf(TEXT("  %s.%s -> %s  (%s)\r\n"), *Binding.ObjectName, *Binding.PropertyName.ToString(),
                *Binding.FunctionName.ToString(), IsDelegateProperty(Widget, Binding.PropertyName) ? TEXT("event") : TEXT("property binding"));
        }
        return Out;
    }

    FString GetWidgetLayout(const UWidgetBlueprint* WidgetBlueprint)
    {
        if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
            return FString();

        const UWidget* RootWidget = WidgetBlueprint->WidgetTree->RootWidget;
        if (!RootWidget)
            return FString();

        const UClass* GeneratedClass = WidgetBlueprint->GeneratedClass;

        FString Hierarchy;
        int32 WidgetCount = 0;
        int32 VariableCount = 0;
        AppendHierarchy(RootWidget, GeneratedClass, FString(), true, Hierarchy, WidgetCount, VariableCount);

        FString Details;
        AppendDetails(RootWidget, GeneratedClass, TEXT("<root>"), Details);

        FString AnimationNames;
        for (const UWidgetAnimation* Animation : WidgetBlueprint->Animations)
        {
            if (!Animation)
                continue;
            if (!AnimationNames.IsEmpty())
                AnimationNames += TEXT(", ");
            AnimationNames += Animation->GetName();
        }

        static const TCHAR* Separator = TEXT("  ----------------------------------------------------------------------------\r\n");

        FString Layout;
        Layout += TEXT("\r\n--[[ ==========================================================================\r\n");
        Layout += TEXT("  UMG STRUCTURE  (auto-generated by UnLua IntelliSense, do not edit)\r\n");
        Layout += TEXT("  [var]     = exposed as a Lua variable, access it with self.<Name>\r\n");
        Layout += TEXT("  [no-var]  = exists in the widget tree only, not reachable from Lua\r\n");
        Layout += TEXT("  props     = properties whose value differs from the widget class default\r\n");
        Layout += TEXT("  slot      = layout data of the parent panel (anchors/offsets/alignment/padding)\r\n");
        Layout += TEXT("  Nested UserWidget children are described in their own IntelliSense file.\r\n");
        Layout += Separator;
        Layout += FString::Printf(TEXT("  Asset       : %s\r\n"), *WidgetBlueprint->GetPathName());
        Layout += FString::Printf(TEXT("  WidgetClass : %s\r\n"), GeneratedClass ? *GeneratedClass->GetName() : TEXT("-"));
        Layout += FString::Printf(TEXT("  NativeParent: %s\r\n"), WidgetBlueprint->ParentClass ? *WidgetBlueprint->ParentClass->GetName() : TEXT("-"));
        Layout += FString::Printf(TEXT("  RootWidget  : %s (%s)\r\n"), *RootWidget->GetName(), *RootWidget->GetClass()->GetName());
        Layout += FString::Printf(TEXT("  Widgets     : %d total, %d bound to Lua, %d not bound\r\n"),
            WidgetCount, VariableCount, WidgetCount - VariableCount);
        if (!AnimationNames.IsEmpty())
            Layout += FString::Printf(TEXT("  Animations  : %d (%s)\r\n"), WidgetBlueprint->Animations.Num(), *AnimationNames);
        Layout += Separator;
        Layout += TEXT("  HIERARCHY\r\n");
        Layout += Hierarchy;
        Layout += Separator;
        Layout += TEXT("  WIDGET DETAILS\r\n");
        Layout += Details;

        const FString Bindings = GetWidgetBindings(WidgetBlueprint);
        if (!Bindings.IsEmpty())
        {
            Layout += Separator;
            Layout += TEXT("  EVENT / PROPERTY BINDINGS  (widget.property -> blueprint function)\r\n");
            Layout += Bindings;
        }

        Layout += TEXT("========================================================================== ]]\r\n");
        return Layout;
    }

    /** Non-default component properties (meshes, materials, lights, transforms). */
    FString GetComponentProperties(const UActorComponent* Component)
    {
        FString Out;
        AppendProperties(Out, Component, {
            TEXT("StaticMesh"), TEXT("SkeletalMesh"), TEXT("OverrideMaterials"), TEXT("Mobility"),
            TEXT("RelativeLocation"), TEXT("RelativeRotation"), TEXT("RelativeScale3D"), TEXT("bVisible"),
            TEXT("CastShadow"), TEXT("Intensity"), TEXT("LightColor"), TEXT("FieldOfView")
        });
        return Out;
    }

    void AppendComponentNode(const USCS_Node* Node, const FString& Prefix, const bool bIsLast, FString& Out)
    {
        if (!Node)
            return;

        Out += FString::Printf(TEXT("%s%s%s (%s)\r\n"), *Prefix, bIsLast ? TEXT("└─ ") : TEXT("├─ "),
            *Node->GetVariableName().ToString(), Node->ComponentClass ? *Node->ComponentClass->GetName() : TEXT("-"));

        if (Node->ComponentTemplate)
        {
            const FString Properties = GetComponentProperties(Node->ComponentTemplate);
            if (!Properties.IsEmpty())
                Out += FString::Printf(TEXT("%s%s  props: %s\r\n"), *Prefix, bIsLast ? TEXT("   ") : TEXT("│  "), *Properties.TrimEnd());
        }

        const TArray<USCS_Node*>& Children = Node->GetChildNodes();
        const FString ChildPrefix = Prefix + (bIsLast ? TEXT("   ") : TEXT("│  "));
        for (int32 Index = 0; Index < Children.Num(); ++Index)
            AppendComponentNode(Children[Index], ChildPrefix, Index == Children.Num() - 1, Out);
    }

    /** Values the blueprint changed in its own Class Defaults panel (i.e. differing from the parent CDO). */
    FString GetClassDefaultOverrides(const UClass* Class)
    {
        FString Out;

        const UClass* ParentClass = Class ? Class->GetSuperClass() : nullptr;
        const UObject* ClassDefaultObject = Class ? Class->GetDefaultObject() : nullptr;
        const UObject* ParentDefaultObject = ParentClass ? ParentClass->GetDefaultObject() : nullptr;
        if (!ClassDefaultObject || !ParentDefaultObject)
            return Out;

        int32 Count = 0;
        for (TFieldIterator<FProperty> It(Class); It; ++It)
        {
            const FProperty* Property = *It;
            // Members declared by this blueprint are already documented as Lua fields above.
            if (Property->GetOwnerClass() == Class || Property->ArrayDim > 1)
                continue;
            // Editor bookkeeping (ActorLabel, bHiddenEdTemporary, ...) is irrelevant for Lua work.
            if (Property->HasAnyPropertyFlags(CPF_Transient | CPF_EditorOnly | CPF_Deprecated))
                continue;

            const FProperty* ParentProperty = ParentClass->FindPropertyByName(Property->GetFName());
            if (!ParentProperty)
                continue;

            const void* Value = Property->ContainerPtrToValuePtr<void>(ClassDefaultObject);
            const void* ParentValue = ParentProperty->ContainerPtrToValuePtr<void>(ParentDefaultObject);
            if (Property->Identical(Value, ParentValue, PPF_None))
                continue;

            // Skip references that point back into this class' own default object (RootComponent, ...).
            if (const FObjectProperty* ObjectProperty = CastField<FObjectProperty>(Property))
            {
                const UObject* ObjectValue = ObjectProperty->GetObjectPropertyValue(Value);
                if (!ObjectValue || ObjectValue->IsIn(ClassDefaultObject))
                    continue;
            }

            FString Text;
            if (const FTextProperty* TextProperty = CastField<FTextProperty>(Property))
                Text = FString::Printf(TEXT("\"%s\""), *TextProperty->GetPropertyValue(Value).ToString());
            else
                Property->ExportTextItem(Text, Value, ParentValue, const_cast<UObject*>(ClassDefaultObject), PPF_None);

            Text = EscapeForComment(CompactNumbers(Text));
            if (Text.IsEmpty())
                continue;
            if (Text.Len() > MaxPropertyTextLength)
            {
                Text.LeftInline(MaxPropertyTextLength);
                Text += TEXT("...<truncated>");
            }

            Out += FString::Printf(TEXT("  %s=%s\r\n"), *Property->GetName(), *Text);
            if (++Count >= 40)
            {
                Out += TEXT("  ... (more overrides omitted)\r\n");
                break;
            }
        }
        return Out;
    }

    /** Event nodes implemented in the event graph (Event BeginPlay, Event Tick, custom events, ...). */
    FString GetImplementedEvents(const UBlueprint* Blueprint)
    {
        FString Out;
        TSet<FString> Seen;

        for (const UEdGraph* Graph : Blueprint->UbergraphPages)
        {
            if (!Graph)
                continue;

            for (const UEdGraphNode* Node : Graph->Nodes)
            {
                const UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node);
                if (!EventNode)
                    continue;

                const FString Title = EventNode->GetNodeTitle(ENodeTitleType::ListView).ToString();
                if (Seen.Contains(Title))
                    continue;

                Seen.Add(Title);
                Out += FString::Printf(TEXT("  %s\r\n"), *Title);
            }
        }
        return Out;
    }

    /** Event Dispatchers declared by this blueprint (multicast delegates), used for BP -> Lua callbacks. */
    FString GetEventDispatchers(const UClass* Class)
    {
        FString Out;
        for (TFieldIterator<FProperty> It(Class); It; ++It)
        {
            const FProperty* Property = *It;
            if (Property->GetOwnerClass() != Class)
                continue;
            if (!CastField<FMulticastDelegateProperty>(Property))
                continue;

            Out += FString::Printf(TEXT("  %s\r\n"), *Property->GetName());
        }
        return Out;
    }

    /** Component tree (SimpleConstructionScript), class default overrides, interfaces and implemented events. */
    FString GetBlueprintStructure(const UBlueprint* Blueprint)
    {
        if (!Blueprint)
            return FString();

        FString Components;
        int32 ComponentCount = 0;
        if (Blueprint->SimpleConstructionScript)
        {
            const TArray<USCS_Node*>& RootNodes = Blueprint->SimpleConstructionScript->GetRootNodes();
            ComponentCount = Blueprint->SimpleConstructionScript->GetAllNodes().Num();
            for (int32 Index = 0; Index < RootNodes.Num(); ++Index)
                AppendComponentNode(RootNodes[Index], FString(), Index == RootNodes.Num() - 1, Components);
        }

        FString Interfaces;
        for (const FBPInterfaceDescription& Description : Blueprint->ImplementedInterfaces)
        {
            if (!Description.Interface)
                continue;
            if (!Interfaces.IsEmpty())
                Interfaces += TEXT(", ");
            Interfaces += Description.Interface->GetName();
        }

        const FString Defaults = GetClassDefaultOverrides(Blueprint->GeneratedClass);
        const FString Events = GetImplementedEvents(Blueprint);
        const FString Dispatchers = GetEventDispatchers(Blueprint->GeneratedClass);

        static const TCHAR* Separator = TEXT("  ----------------------------------------------------------------------------\r\n");

        FString Out;
        Out += TEXT("\r\n--[[ ==========================================================================\r\n");
        Out += TEXT("  BLUEPRINT STRUCTURE  (auto-generated by UnLua IntelliSense, do not edit)\r\n");
        Out += TEXT("  COMPONENT TREE = components created by this blueprint (SimpleConstructionScript)\r\n");
        Out += TEXT("  props          = component values that differ from the component class default\r\n");
        Out += TEXT("  CLASS DEFAULTS  = only values changed in this blueprint's Class Defaults panel\r\n");
        Out += Separator;
        Out += FString::Printf(TEXT("  Asset         : %s\r\n"), *Blueprint->GetPathName());
        Out += FString::Printf(TEXT("  BlueprintClass: %s\r\n"), Blueprint->GeneratedClass ? *Blueprint->GeneratedClass->GetName() : TEXT("-"));
        Out += FString::Printf(TEXT("  ParentClass   : %s\r\n"), Blueprint->ParentClass ? *Blueprint->ParentClass->GetName() : TEXT("-"));
        Out += FString::Printf(TEXT("  Interfaces    : %s\r\n"), Interfaces.IsEmpty() ? TEXT("-") : *Interfaces);
        Out += FString::Printf(TEXT("  Components    : %d\r\n"), ComponentCount);
        Out += Separator;

        if (!Components.IsEmpty())
        {
            Out += TEXT("  COMPONENT TREE\r\n");
            Out += Components;
            Out += Separator;
        }

        if (!Defaults.IsEmpty())
        {
            Out += TEXT("  CLASS DEFAULTS (differs from parent class)\r\n");
            Out += Defaults;
            Out += Separator;
        }

        if (!Events.IsEmpty())
        {
            Out += TEXT("  IMPLEMENTED EVENTS\r\n");
            Out += Events;
            Out += Separator;
        }

        if (!Dispatchers.IsEmpty())
        {
            Out += TEXT("  EVENT DISPATCHERS (bind from Lua: self:<Name>:Add(fn))\r\n");
            Out += Dispatchers;
            Out += Separator;
        }

        Out += TEXT("========================================================================== ]]\r\n");
        return Out;
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
    const UUnLuaEditorSettings* StructureSettings = GetDefault<UUnLuaEditorSettings>();
    if (StructureSettings->bGenerateWidgetLayout != bCachedGenerateWidgetLayout
        || StructureSettings->bGenerateBlueprintStructure != bCachedGenerateBlueprintStructure)
    {
        AssetCache.Reset();
        bManifestDirty = true;
        bCachedGenerateWidgetLayout = StructureSettings->bGenerateWidgetLayout;
        bCachedGenerateBlueprintStructure = StructureSettings->bGenerateBlueprintStructure;
    }

    const FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

    FARFilter Filter;
    Filter.ClassNames.Add(UBlueprint::StaticClass()->GetFName());
    Filter.ClassNames.Add(UWidgetBlueprint::StaticClass()->GetFName());

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
                UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetData.ObjectPath.ToString());
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
    const FName AssetClass = AssetData.AssetClass;
    return AssetClass == UBlueprint::StaticClass()->GetFName() || AssetClass == UWidgetBlueprint::StaticClass()->GetFName();
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
    if (const UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Blueprint))
    {
        if (Settings->bGenerateWidgetLayout)
            Content += GetWidgetLayout(WidgetBlueprint);
    }
    else if (Settings->bGenerateBlueprintStructure)
    {
        Content += GetBlueprintStructure(Blueprint);
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

    const UUnLuaEditorSettings* StructureSettings = GetDefault<UUnLuaEditorSettings>();
    bool bManifestGenerateWidgetLayout = false;
    if (!RootObject->TryGetBoolField(TEXT("GenerateWidgetLayout"), bManifestGenerateWidgetLayout)
        || bManifestGenerateWidgetLayout != StructureSettings->bGenerateWidgetLayout)
    {
        bManifestDirty = true;
        return false;
    }

    bool bManifestGenerateBlueprintStructure = false;
    if (!RootObject->TryGetBoolField(TEXT("GenerateBlueprintStructure"), bManifestGenerateBlueprintStructure)
        || bManifestGenerateBlueprintStructure != StructureSettings->bGenerateBlueprintStructure)
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
    RootObject->SetBoolField(TEXT("GenerateBlueprintStructure"), GetDefault<UUnLuaEditorSettings>()->bGenerateBlueprintStructure);

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
    if (!FPackageName::DoesPackageExist(AssetData.PackageName.ToString(), nullptr, &PackageFilename))
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
    const FString ObjectPath = AssetData.ObjectPath.ToString();
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

    FUnLuaIntelliSenseAssetCache& CacheEntry = AssetCache.FindOrAdd(AssetData.ObjectPath.ToString());
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

    const FString ObjectPath = AssetData.ObjectPath.ToString();
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

    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetData.ObjectPath.ToString());
    if (!Blueprint)
        return;

    const FString OutputFile = Export(Blueprint);
    if (!OutputFile.IsEmpty())
    {
        UpdateAssetCache(AssetData, OutputFile);
        SaveManifest();
    }
}
