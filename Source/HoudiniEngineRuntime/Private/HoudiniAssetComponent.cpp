/*
* Copyright (c) <2018> Side Effects Software Inc.
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are met:
*
* 1. Redistributions of source code must retain the above copyright notice,
*    this list of conditions and the following disclaimer.
*
* 2. The name of Side Effects Software may not be used to endorse or
*    promote products derived from this software without specific prior
*    written permission.
*
* THIS SOFTWARE IS PROVIDED BY SIDE EFFECTS SOFTWARE "AS IS" AND ANY EXPRESS
* OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
* OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN
* NO EVENT SHALL SIDE EFFECTS SOFTWARE BE LIABLE FOR ANY DIRECT, INDIRECT,
* INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
* LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
* OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
* LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
* NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
* EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "HoudiniAssetComponent.h"

#include "HoudiniAsset.h"
#include "HoudiniAssetActor.h"
#include "HoudiniInput.h"
#include "HoudiniOutput.h"
#include "HoudiniParameter.h"
#include "HoudiniHandleComponent.h"
#include "HoudiniPDGAssetLink.h"
#include "HoudiniEngineRuntime.h"
#include "HoudiniStaticMeshComponent.h"

#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "TimerManager.h"
#include "Landscape.h"

/*
#if WITH_EDITOR
	#include "Editor/UnrealEd/Public/FileHelper.h"
#endif
*/

UHoudiniAssetComponent::UHoudiniAssetComponent(const FObjectInitializer & ObjectInitializer)
	: Super(ObjectInitializer)
{
	HoudiniAsset = nullptr;	
	bCookOnParameterChange = true;
	bUploadTransformsToHoudiniEngine = true;
	bCookOnTransformChange = false;
	//bUseNativeHoudiniMaterials = true;
	bCookOnAssetInputCook = true;

	AssetId = -1;

	// Make an invalid GUID, since we do not have any cooking requests.
	HapiGUID.Invalidate();

	// Create unique component GUID.
	ComponentGUID = FGuid::NewGuid();

	AssetState = EHoudiniAssetState::PreInstantiation;
	AssetStateResult = EHoudiniAssetStateResult::None;
	
	SubAssetIndex = -1;

	bUploadTransformsToHoudiniEngine = true;

	bHasBeenLoaded = false;
	bHasBeenDuplicated = false;
	bPendingDelete = false;
	bRecookRequested = false;
	bRebuildRequested = false;
	bEnableCooking = true;

	//bEditorPropertiesNeedFullUpdate = true;

	// Folder used for cooking
	TemporaryCookFolder.Path = HAPI_UNREAL_DEFAULT_TEMP_COOK_FOLDER;
	
	// Folder used for baking this asset's outputs
	BakeFolder.Path = HAPI_UNREAL_DEFAULT_BAKE_FOLDER;

	bHasComponentTransformChanged = false;

	bFullyLoaded = false;

	bOutputless = false;

	PDGAssetLink = nullptr;

	StaticMeshMethod = EHoudiniStaticMeshMethod::RawMesh;

	const UHoudiniRuntimeSettings * HoudiniRuntimeSettings = GetDefault< UHoudiniRuntimeSettings >();
	if (HoudiniRuntimeSettings)
	{
		bEnableProxyStaticMeshOverride = HoudiniRuntimeSettings->bEnableProxyStaticMesh;
		bEnableProxyStaticMeshRefinementByTimerOverride = HoudiniRuntimeSettings->bEnableProxyStaticMeshRefinementByTimer;
		ProxyMeshAutoRefineTimeoutSecondsOverride = HoudiniRuntimeSettings->ProxyMeshAutoRefineTimeoutSeconds;
		bEnableProxyStaticMeshRefinementOnPreSaveWorldOverride = HoudiniRuntimeSettings->bEnableProxyStaticMeshRefinementOnPreSaveWorld;
		bEnableProxyStaticMeshRefinementOnPreBeginPIEOverride = HoudiniRuntimeSettings->bEnableProxyStaticMeshRefinementOnPreBeginPIE;
	}

	bNoProxyMeshNextCookRequested = false;

#if WITH_EDITORONLY_DATA
	bGenerateMenuExpanded = true;
	bBakeMenuExpanded = true;
	bAssetOptionMenuExpanded = true;
	bHelpAndDebugMenuExpanded = true;

	bIsReplace = false;

	HoudiniEngineBakeOption = EHoudiniEngineBakeOption::ToActor;
#endif
}

UHoudiniAssetComponent::~UHoudiniAssetComponent()
{
	// Unregister ourself so our houdini node can be delete.
	FHoudiniEngineRuntime::Get().MarkNodeIdAsPendingDelete(AssetId, true);
	FHoudiniEngineRuntime::Get().UnRegisterHoudiniComponent(this);
}

void UHoudiniAssetComponent::PostInitProperties()
{
	Super::PostInitProperties();

	// Register ourself to the HER singleton
	FHoudiniEngineRuntime::Get().RegisterHoudiniComponent(this);
}

UHoudiniAsset *
UHoudiniAssetComponent::GetHoudiniAsset() const
{
	return HoudiniAsset;
}

FString
UHoudiniAssetComponent::GetDisplayName() const
{
	return GetOwner() ? GetOwner()->GetName() : GetName();
}

bool 
UHoudiniAssetComponent::IsProxyStaticMeshEnabled() const
{
	if (bOverrideGlobalProxyStaticMeshSettings)
	{
		return bEnableProxyStaticMeshOverride;
	}
	else
	{
		const UHoudiniRuntimeSettings * HoudiniRuntimeSettings = GetDefault< UHoudiniRuntimeSettings >();
		if (HoudiniRuntimeSettings)
		{
			return HoudiniRuntimeSettings->bEnableProxyStaticMesh;
		}
		else
		{
			return false;
		}
	}
}

bool 
UHoudiniAssetComponent::IsProxyStaticMeshRefinementByTimerEnabled() const
{
	if (bOverrideGlobalProxyStaticMeshSettings)
	{
		return bEnableProxyStaticMeshOverride && bEnableProxyStaticMeshRefinementByTimerOverride;
	}
	else
	{
		const UHoudiniRuntimeSettings * HoudiniRuntimeSettings = GetDefault< UHoudiniRuntimeSettings >();
		if (HoudiniRuntimeSettings)
		{
			return HoudiniRuntimeSettings->bEnableProxyStaticMesh && HoudiniRuntimeSettings->bEnableProxyStaticMeshRefinementByTimer;
		}
		else
		{
			return false;
		}
	}
}

float
UHoudiniAssetComponent::GetProxyMeshAutoRefineTimeoutSeconds() const
{
	if (bOverrideGlobalProxyStaticMeshSettings)
	{
		return ProxyMeshAutoRefineTimeoutSecondsOverride;
	}
	else
	{
		const UHoudiniRuntimeSettings * HoudiniRuntimeSettings = GetDefault< UHoudiniRuntimeSettings >();
		if (HoudiniRuntimeSettings)
		{
			return HoudiniRuntimeSettings->ProxyMeshAutoRefineTimeoutSeconds;
		}
		else
		{
			return 5.0f;
		}
	}
}

bool
UHoudiniAssetComponent::IsProxyStaticMeshRefinementOnPreSaveWorldEnabled() const
{
	if (bOverrideGlobalProxyStaticMeshSettings)
	{
		return bEnableProxyStaticMeshOverride && bEnableProxyStaticMeshRefinementOnPreSaveWorldOverride;
	}
	else
	{
		const UHoudiniRuntimeSettings * HoudiniRuntimeSettings = GetDefault< UHoudiniRuntimeSettings >();
		if (HoudiniRuntimeSettings)
		{
			return HoudiniRuntimeSettings->bEnableProxyStaticMesh && HoudiniRuntimeSettings->bEnableProxyStaticMeshRefinementOnPreSaveWorld;
		}
		else
		{
			return false;
		}
	}
}

bool 
UHoudiniAssetComponent::IsProxyStaticMeshRefinementOnPreBeginPIEEnabled() const
{
	if (bOverrideGlobalProxyStaticMeshSettings)
	{
		return bEnableProxyStaticMeshOverride && bEnableProxyStaticMeshRefinementOnPreBeginPIEOverride;
	}
	else
	{
		const UHoudiniRuntimeSettings * HoudiniRuntimeSettings = GetDefault< UHoudiniRuntimeSettings >();
		if (HoudiniRuntimeSettings)
		{
			return HoudiniRuntimeSettings->bEnableProxyStaticMesh && HoudiniRuntimeSettings->bEnableProxyStaticMeshRefinementOnPreBeginPIE;
		}
		else
		{
			return false;
		}
	}
}

void
UHoudiniAssetComponent::SetHoudiniAsset(UHoudiniAsset * InHoudiniAsset)
{
	// Check the asset validity
	if (!InHoudiniAsset || InHoudiniAsset->IsPendingKill())
		return;

	// If it is the same asset, do nothing.
	if ( InHoudiniAsset == HoudiniAsset )
		return;

	HoudiniAsset = InHoudiniAsset;
}


void 
UHoudiniAssetComponent::OnHoudiniAssetChanged()
{
	// TODO: clear input/params/outputs?

	// The asset has been changed, mark us as needing to be reinstantiated
	MarkAsNeedInstantiation();
}

bool
UHoudiniAssetComponent::NeedUpdate() const
{
	// We must have a valid asset
	if (!HoudiniAsset || HoudiniAsset->IsPendingKill())
		return false;
	
	// If we don't want to cook on parameter/input change dont bother looking for updates
	if (!bCookOnParameterChange && !bRecookRequested && !bRebuildRequested)
		return false;

	// Check if the HAC's transform has changed and transform triggers cook is enabled
	if (bCookOnTransformChange && bHasComponentTransformChanged)
		return true;

	// Go through all our parameters, return true if they have been updated
	for (auto CurrentParm : Parameters)
	{
		if (!CurrentParm || CurrentParm->IsPendingKill())
			continue;

		if (!CurrentParm->HasChanged())
			continue;

		// See if the parameter doesn't require an update 
		// (because it has failed to upload previously or has been loaded)
		if (!CurrentParm->NeedsToTriggerUpdate())
			continue;

		return true;
	}

	// Go through all our inputs, return true if they have been updated
	for (auto CurrentInput : Inputs)
	{
		if (!CurrentInput || CurrentInput->IsPendingKill())
			continue;

		if (!CurrentInput->HasChanged())
			continue;

		// See if the input doesn't require an update 
		// (because it has failed to upload previously or has been loaded)
		if (!CurrentInput->NeedsToTriggerUpdate())
			continue;

		return true;
	}

	// Go through all outputs, filter the editable nodes. Return true if they have been updated.
	for (auto CurrentOutput : Outputs) 
	{
		if (!CurrentOutput || CurrentOutput->IsPendingKill())
			continue;
		
		// We only care about editable outputs
		if (!CurrentOutput->IsEditableNode())
			continue;

		// Trigger an update if the output object is marked as modified by user.
		TMap<FHoudiniOutputObjectIdentifier, FHoudiniOutputObject>& OutputObjects = CurrentOutput->GetOutputObjects();
		for (auto& NextPair : OutputObjects)
		{
			// For now, only editable curves can trigger update
			UHoudiniSplineComponent* HoudiniSplineComponent = Cast<UHoudiniSplineComponent>(NextPair.Value.OutputComponent);
			if (!HoudiniSplineComponent)
				continue;

			// Output curves cant trigger an update!
			if (HoudiniSplineComponent->bIsOutputCurve)
				continue;

			if (HoudiniSplineComponent->NeedsToTrigerUpdate())
				return true;
		}
	}

	return false;
}

// Indicates if any of the HAC's output components needs to be updated (no recook needed)
bool
UHoudiniAssetComponent::NeedOutputUpdate() const
{
	// Go through all outputs
	for (auto CurrentOutput : Outputs)
	{
		if (!CurrentOutput || CurrentOutput->IsPendingKill())
			continue;

		for (const auto& InstOutput : CurrentOutput->GetInstancedOutputs())
		{
			if (InstOutput.Value.bChanged)
				return true;
		}

		// Check if any output curve's export type has been changed
		if (CurrentOutput->HasCurveExportTypeChanged())
			return true;
	}

	return false;
}

bool 
UHoudiniAssetComponent::NotifyCookedToDownstreamAssets()
{
	// Before notifying, clean up our downstream assets
	// - check that they are still valid
	// - check that we are still connected to one of its asset input
	// - check that the asset as the CookOnAssetInputCook trigger enabled
	TArray<UHoudiniAssetComponent*> DownstreamToDelete;	
	for(auto& CurrentDownstreamHAC : DownstreamHoudiniAssets)
	{
		// Remove the downstream connection by default,
		// unless we actually were properly connected to one of this HDa's input.
		bool bRemoveDownstream = true;
		if (CurrentDownstreamHAC && !CurrentDownstreamHAC->IsPendingKill())
		{
			// Go through the HAC's input
			for (auto& CurrentDownstreamInput : CurrentDownstreamHAC->Inputs)
			{
				if (!CurrentDownstreamInput || CurrentDownstreamInput->IsPendingKill())
					continue;

				if (CurrentDownstreamInput->GetInputType() != EHoudiniInputType::Asset)
					continue;

				if (!CurrentDownstreamInput->ContainsInputObject(this, EHoudiniInputType::Asset))
					continue;
								
				if (CurrentDownstreamHAC->bCookOnAssetInputCook)
				{
					// Mark that HAC's input has changed
					CurrentDownstreamInput->MarkChanged(true);
				}				
				bRemoveDownstream = false;
			}
		}

		if (bRemoveDownstream)
		{
			DownstreamToDelete.Add(CurrentDownstreamHAC);
		}
	}

	for (auto ToDelete : DownstreamToDelete)
	{
		DownstreamHoudiniAssets.Remove(ToDelete);
	}

	return true;
}

bool
UHoudiniAssetComponent::NeedsToWaitForInputHoudiniAssets()
{
	bool bNeedToWait = false;
	for (auto& CurrentInput : Inputs)
	{
		if (!CurrentInput || CurrentInput->IsPendingKill() || CurrentInput->GetInputType() != EHoudiniInputType::Asset)
			continue;

		TArray<UHoudiniInputObject*>* ObjectArray = CurrentInput->GetHoudiniInputObjectArray(EHoudiniInputType::Asset);
		if (!ObjectArray)
			continue;

		for (auto& CurrentInputObject : (*ObjectArray))
		{
			// Get the input HDA
			UHoudiniAssetComponent* InputHAC = CurrentInputObject 
				? Cast<UHoudiniAssetComponent>(CurrentInputObject->GetObject()) 
				: nullptr;

			if (!InputHAC)
				continue;

			// If the input HDA needs to be instantiated, force him to instantiate
			// if the input HDA is in any other state than None, we need to wait for him
			// to finish whatever it's doing
			if (InputHAC->GetAssetState() == EHoudiniAssetState::NeedInstantiation)
			{
				// Tell the input HAC to instantiate
				InputHAC->AssetState = EHoudiniAssetState::PreInstantiation;
				// We need to wait
				bNeedToWait = true;
			}
			else if (InputHAC->GetAssetState() != EHoudiniAssetState::None)
			{
				// We need to wait
				bNeedToWait = true;
			}
		}
	}

	return bNeedToWait;
}

void
UHoudiniAssetComponent::BeginDestroy()
{
	// Unregister ourself so our houdini node can be deleted
	FHoudiniEngineRuntime::Get().MarkNodeIdAsPendingDelete(AssetId, true);
	FHoudiniEngineRuntime::Get().UnRegisterHoudiniComponent(this);

	Super::BeginDestroy();
}

void 
UHoudiniAssetComponent::MarkAsNeedCook()
{
	// Force the asset state to NeedCook
	//AssetCookCount = 0;
	bHasBeenLoaded = true;
	bPendingDelete = false;
	bRecookRequested = true;
	bRebuildRequested = false;

	//bEditorPropertiesNeedFullUpdate = true;

	// We need to mark all our parameters as changed/trigger update
	for (auto CurrentParam : Parameters)
	{
		CurrentParam->MarkChanged(true);
		CurrentParam->SetNeedsToTriggerUpdate(true);
	}

	// We need to mark all our inputs as changed/trigger update
	for (auto CurrentInput : Inputs)
	{
		CurrentInput->MarkChanged(true);
		CurrentInput->SetNeedsToTriggerUpdate(true);
		CurrentInput->MarkDataUploadNeeded(true);
	}

	// Clear the static mesh bake timer
	ClearRefineMeshesTimer();
}

void
UHoudiniAssetComponent::MarkAsNeedRebuild()
{
	// Invalidate the asset ID
	//AssetId = -1;

	// Force the asset state to NeedRebuild
	AssetState = EHoudiniAssetState::NeedRebuild;
	AssetStateResult = EHoudiniAssetStateResult::None;

	// Reset some of the asset's flag
	//AssetCookCount = 0;
	bHasBeenLoaded = true;
	bPendingDelete = false;
	bRecookRequested = false;
	bRebuildRequested = true;
	bFullyLoaded = false;

	//bEditorPropertiesNeedFullUpdate = true;

	// We need to mark all our parameters as changed/trigger update
	for (auto CurrentParam : Parameters)
	{
		CurrentParam->MarkChanged(true);
		CurrentParam->SetNeedsToTriggerUpdate(true);
	}

	// We need to mark all our inputs as changed/trigger update
	for (auto CurrentInput : Inputs)
	{
		CurrentInput->MarkChanged(true);
		CurrentInput->SetNeedsToTriggerUpdate(true);
		CurrentInput->MarkDataUploadNeeded(true);
	}

	// Clear the static mesh bake timer
	ClearRefineMeshesTimer();
}

// Marks the asset as needing to be instantiated
void
UHoudiniAssetComponent::MarkAsNeedInstantiation()
{
	// Invalidate the asset ID
	AssetId = -1;

	if (Parameters.Num() <= 0 && Inputs.Num() <= 0 && Outputs.Num() <= 0)
	{
		// The asset has no parameters or inputs.
		// This likely indicates it has never cooked/been instantiated.
		// Set its state to PreInstantiation to force its instantiation
		// so that we can have its parameters/input interface
		AssetState = EHoudiniAssetState::PreInstantiation;
	}
	else
	{
		// The asset has cooked before since we have a parameter/input interface
		// Set its state to need instantiation so that the asset is instantiated
		// after being modified
		AssetState = EHoudiniAssetState::NeedInstantiation;
	}

	AssetStateResult = EHoudiniAssetStateResult::None;

	// Reset some of the asset's flag
	AssetCookCount = 0;
	bHasBeenLoaded = true;
	bPendingDelete = false;
	bRecookRequested = false;
	bRebuildRequested = false;
	bFullyLoaded = false;

	//bEditorPropertiesNeedFullUpdate = true;

	// We need to mark all our parameters as changed/not triggering update
	for (auto CurrentParam : Parameters)
	{
		if (CurrentParam)
		{
			CurrentParam->MarkChanged(true);
			CurrentParam->SetNeedsToTriggerUpdate(false);
		}
	}

	// We need to mark all our inputs as changed/not triggering update
	for (auto CurrentInput : Inputs)
	{
		if (CurrentInput)
		{
			CurrentInput->MarkChanged(true);
			CurrentInput->SetNeedsToTriggerUpdate(false);
			CurrentInput->MarkDataUploadNeeded(true);
		}
	}

	// Clear the static mesh bake timer
	ClearRefineMeshesTimer();
}

void
UHoudiniAssetComponent::PostLoad()
{
	Super::PostLoad();

	MarkAsNeedInstantiation();

	// Component has been loaded, not duplicated
	bHasBeenDuplicated = false;

	// We need to register ourself
	FHoudiniEngineRuntime::Get().RegisterHoudiniComponent(this);

	// Register our PDG Asset link if we have any

}

void 
UHoudiniAssetComponent::PostEditImport()
{
	Super::PostEditImport();

	MarkAsNeedInstantiation();

	// Component has been duplicated, not loaded
	// We do need the loaded flag to reapply parameters, inputs
	// and properly update some of the output objects
	bHasBeenDuplicated = true;

	//RemoveAllAttachedComponents();

	AssetState = EHoudiniAssetState::PreInstantiation;
	AssetStateResult = EHoudiniAssetStateResult::None;
	
	// TODO?
	// REGISTER?
}

void
UHoudiniAssetComponent::UpdatePostDuplicate()
{
	// TODO:
	// - Keep the output objects/components (remove duplicatetransient on the output object uproperties)
	// - Duplicate created objects (ie SM) and materials
	// - Update the output components to use these instead
	// This should remove the need for a cook on duplicate

	// For now, we simply clean some of the HAC's component manually
	const TArray<USceneComponent*> Children = GetAttachChildren();

	for (auto & NextChild : Children) 
	{
		if (!NextChild || NextChild->IsPendingKill())
			continue;

		USceneComponent * ComponentToRemove = nullptr;
		if (NextChild->IsA<UStaticMeshComponent>()) 
		{
			ComponentToRemove = NextChild;
		}
		else if (NextChild->IsA<UHoudiniStaticMeshComponent>())
		{
			ComponentToRemove = NextChild;
		}
		else if (NextChild->IsA<UHoudiniSplineComponent>())  
		{
			// Remove duplicated editable curve output's Houdini Spline Component, since they will be re-built at duplication.
			UHoudiniSplineComponent * HoudiniSplineComponent = Cast<UHoudiniSplineComponent>(NextChild);
			if (HoudiniSplineComponent && HoudiniSplineComponent->IsEditableOutputCurve())
				ComponentToRemove = NextChild;
		}

		if (ComponentToRemove)
		{
			ComponentToRemove->DetachFromComponent(FDetachmentTransformRules::KeepRelativeTransform);
			ComponentToRemove->UnregisterComponent();
			ComponentToRemove->DestroyComponent();
		}
	}

	SetHasBeenDuplicated(false);
}

void
UHoudiniAssetComponent::OnComponentCreated()
{
	// This event will only be fired for native Actor and native Component.
 	Super::OnComponentCreated();

	if (!GetOwner() || !GetOwner()->GetWorld())
		return;
	/*
	if (StaticMeshes.Num() == 0)
	{
		// Create Houdini logo static mesh and component for it.
		CreateStaticMeshHoudiniLogoResource(StaticMeshes);
	}

	// Create replacement material object.
	if (!HoudiniAssetComponentMaterials)
	{
		HoudiniAssetComponentMaterials =
			NewObject< UHoudiniAssetComponentMaterials >(
				this, UHoudiniAssetComponentMaterials::StaticClass(), NAME_None, RF_Public | RF_Transactional);
	}
	*/
}

void
UHoudiniAssetComponent::OnComponentDestroyed(bool bDestroyingHierarchy)
{
	// Unregister ourself so our houdini node can be deleted
	FHoudiniEngineRuntime::Get().UnRegisterHoudiniComponent(this);

	HoudiniAsset = nullptr;

	// Clear Parameters
	for (UHoudiniParameter*& CurrentParm : Parameters)
	{
		if (CurrentParm && !CurrentParm->IsPendingKill())
		{
			CurrentParm->ConditionalBeginDestroy();
		}
		else if (GetWorld() != NULL && GetWorld()->WorldType != EWorldType::PIE)
		{
			// TODO unneeded log?
			// Avoid spamming that error when leaving PIE mode
			HOUDINI_LOG_WARNING(TEXT("%s: null parameter when clearing"), GetOwner() ? *(GetOwner()->GetName()) : *GetName());
		}

		CurrentParm = nullptr;
	}

	Parameters.Empty();

	// Clear Inputs
	for (UHoudiniInput*&  CurrentInput : Inputs)
	{
		if (!CurrentInput || CurrentInput->IsPendingKill())
			continue;

		if (CurrentInput->HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad))
			continue;

		// Destroy connected Houdini asset.
		CurrentInput->ConditionalBeginDestroy();
		CurrentInput = nullptr;
	}

	Inputs.Empty();

	// Clear Output
	for (UHoudiniOutput*& CurrentOutput : Outputs)
	{
		if (!CurrentOutput || CurrentOutput->IsPendingKill())
			continue;

		if (CurrentOutput->HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad))
			continue;

		CurrentOutput->Clear();
		// Destroy connected Houdini asset.
		CurrentOutput->ConditionalBeginDestroy();
		CurrentOutput = nullptr;
	}

	Outputs.Empty();

	// Unregister ourself so our houdini node can be delete.
	FHoudiniEngineRuntime::Get().MarkNodeIdAsPendingDelete(AssetId, true);
	FHoudiniEngineRuntime::Get().UnRegisterHoudiniComponent(this);

	// Clear the static mesh bake timer
	ClearRefineMeshesTimer();

	Super::OnComponentDestroyed(bDestroyingHierarchy);
}

void
UHoudiniAssetComponent::OnRegister()
{
	Super::OnRegister();

	/*
	// We need to recreate render states for loaded components.
	if (bLoadedComponent)
	{
		// Static meshes.
		for (TMap< UStaticMesh *, UStaticMeshComponent * >::TIterator Iter(StaticMeshComponents); Iter; ++Iter)
		{
			UStaticMeshComponent * StaticMeshComponent = Iter.Value();
			if (StaticMeshComponent && !StaticMeshComponent->IsPendingKill())
			{
				// Recreate render state.
				StaticMeshComponent->RecreateRenderState_Concurrent();

				// Need to recreate physics state.
				StaticMeshComponent->RecreatePhysicsState();
			}
		}

		// Instanced static meshes.
		for (auto& InstanceInput : InstanceInputs)
		{
			if (!InstanceInput || InstanceInput->IsPendingKill())
				continue;

			// Recreate render state.
			InstanceInput->RecreateRenderStates();

			// Recreate physics state.
			InstanceInput->RecreatePhysicsStates();
		}
	}
	*/

	// We can now consider the asset as fully loaded
	bFullyLoaded = true;
}

UHoudiniParameter*
UHoudiniAssetComponent::FindMatchingParameter(UHoudiniParameter* InOtherParam)
{
	if (!InOtherParam || InOtherParam->IsPendingKill())
		return nullptr;

	for (auto CurrentParam : Parameters)
	{
		if (!CurrentParam || CurrentParam->IsPendingKill())
			continue;

		if (CurrentParam->Matches(*InOtherParam))
			return CurrentParam;
	}

	return nullptr;
}

UHoudiniInput*
UHoudiniAssetComponent::FindMatchingInput(UHoudiniInput* InOtherInput)
{
	if (!InOtherInput || InOtherInput->IsPendingKill())
		return nullptr;

	for (auto CurrentInput : Inputs)
	{
		if (!CurrentInput || CurrentInput->IsPendingKill())
			continue;

		if (CurrentInput->Matches(*InOtherInput))
			return CurrentInput;
	}

	return nullptr;
}

UHoudiniHandleComponent* 
UHoudiniAssetComponent::FindMatchingHandle(UHoudiniHandleComponent* InOtherHandle) 
{
	if (!InOtherHandle || InOtherHandle->IsPendingKill())
		return nullptr;

	for (auto CurrentHandle : HandleComponents) 
	{
		if (!CurrentHandle || CurrentHandle->IsPendingKill())
			continue;

		if (CurrentHandle->Matches(*InOtherHandle))
			return CurrentHandle;
	}

	return nullptr;
}

UHoudiniParameter*
UHoudiniAssetComponent::FindParameterByName(const FString& InParamName)
{
	for (auto CurrentParam : Parameters)
	{
		if (!CurrentParam || CurrentParam->IsPendingKill())
			continue;

		if (CurrentParam->GetParameterName().Equals(InParamName))
			return CurrentParam;
	}

	return nullptr;
}


void
UHoudiniAssetComponent::OnChildAttached(USceneComponent* ChildComponent)
{
	Super::OnChildAttached(ChildComponent);

	// ... Do corresponding things for other houdini component types.
	// ...
}


void
UHoudiniAssetComponent::OnUpdateTransform(EUpdateTransformFlags UpdateTransformFlags, ETeleportType Teleport)
{
	Super::OnUpdateTransform(UpdateTransformFlags, Teleport);

	SetHasComponentTransformChanged(true);
}


#if WITH_EDITOR
void
UHoudiniAssetComponent::PostEditChangeProperty(FPropertyChangedEvent & PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	FProperty * Property = PropertyChangedEvent.MemberProperty;
	if (!Property)
		return;

	FName PropertyName = Property->GetFName();
	
	// Changing the Houdini Asset?
	if (PropertyName == GET_MEMBER_NAME_CHECKED(UHoudiniAssetComponent, HoudiniAsset))
	{
		OnHoudiniAssetChanged();
	}
	else if (PropertyName == GetRelativeLocationPropertyName()
			|| PropertyName == GetRelativeRotationPropertyName()
			|| PropertyName == GetRelativeScale3DPropertyName())
	{
		SetHasComponentTransformChanged(true);
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UHoudiniAssetComponent, bOverrideGlobalProxyStaticMeshSettings)
			|| PropertyName == GET_MEMBER_NAME_CHECKED(UHoudiniAssetComponent, bEnableProxyStaticMeshRefinementByTimerOverride)
			|| PropertyName == GET_MEMBER_NAME_CHECKED(UHoudiniAssetComponent, ProxyMeshAutoRefineTimeoutSecondsOverride))
	{
		ClearRefineMeshesTimer();
		// Reset the timer
		// SetRefineMeshesTimer will check the relevant settings and only set the timer if enabled via settings
		SetRefineMeshesTimer();
	}
	else
	{
		// TODO:
		// Propagate properties (mobility/visibility etc.. to children components)
	}
}
#endif


#if WITH_EDITOR
void
UHoudiniAssetComponent::PostEditUndo()
{
	Super::PostEditUndo();

	if (!IsPendingKill())
	{
		// Make sure we are registered with the HER singleton
		// We could be undoing a HoudiniActor delete
		if (!FHoudiniEngineRuntime::Get().IsComponentRegistered(this))
		{
			MarkAsNeedInstantiation();

			// Component has been loaded, not duplicated
			bHasBeenDuplicated = false;

			FHoudiniEngineRuntime::Get().RegisterHoudiniComponent(this);
		}
	}
}
#endif


#if WITH_EDITOR
void
UHoudiniAssetComponent::OnActorMoved(AActor* Actor)
{
	if (GetOwner() != Actor)
		return;

	SetHasComponentTransformChanged(true);
}
#endif

void 
UHoudiniAssetComponent::SetHasComponentTransformChanged(const bool& InHasChanged)
{
	// Only update the value if we're fully loaded
	// This avoid triggering a recook when loading a level
	if(bFullyLoaded)
		bHasComponentTransformChanged = InHasChanged;
}

void
UHoudiniAssetComponent::SetPDGAssetLink(UHoudiniPDGAssetLink* InPDGAssetLink)
{
	// Check the object validity
	if (!InPDGAssetLink || InPDGAssetLink->IsPendingKill())
		return;

	// If it is the same object, do nothing.
	if (InPDGAssetLink == PDGAssetLink)
		return;

	PDGAssetLink = InPDGAssetLink;
}


FBoxSphereBounds
UHoudiniAssetComponent::CalcBounds(const FTransform & LocalToWorld) const
{

	return Super::CalcBounds(LocalToWorld);

	/*
	// TODO: FINISH ME!
	FBoxSphereBounds LocalBounds;
	FBox BoundingBox = GetAssetBounds();
	if (BoundingBox.GetExtent() == FVector::ZeroVector)
		BoundingBox.ExpandBy(1.0f);

	LocalBounds = FBoxSphereBounds(BoundingBox);
	// fix for offset bounds - maintain local bounds origin
	LocalBounds.TransformBy(LocalToWorld);

	const auto & LocalAttachedChildren = GetAttachChildren();
	for (int32 Idx = 0; Idx < LocalAttachedChildren.Num(); ++Idx)
	{
		if (!LocalAttachedChildren[Idx])
			continue;

		FBoxSphereBounds ChildBounds = LocalAttachedChildren[Idx]->CalcBounds(LocalToWorld);
		if (!ChildBounds.ContainsNaN())
			LocalBounds = LocalBounds + ChildBounds;
	}

	return LocalBounds;
	*/
}


FBox
UHoudiniAssetComponent::GetAssetBounds(UHoudiniInput* IgnoreInput, const bool& bIgnoreGeneratedLandscape) const
{
	// TODO: FINISH ME!
	FBox BoxBounds(ForceInitToZero);

	/*
	// Query the bounds of all our static mesh components..
	for (TMap< UStaticMesh *, UStaticMeshComponent * >::TConstIterator Iter(StaticMeshComponents); Iter; ++Iter)
	{
		UStaticMeshComponent * StaticMeshComponent = Iter.Value();
		if (!StaticMeshComponent || StaticMeshComponent->IsPendingKill())
			continue;

		FBox StaticMeshBounds = StaticMeshComponent->Bounds.GetBox();
		if (StaticMeshBounds.IsValid)
			BoxBounds += StaticMeshBounds;
	}

	// Also scan all our decendants for SMC bounds not just top-level children
	// ( split mesh instances' mesh bounds were not gathered proiperly )
	TArray<USceneComponent*> LocalAttachedChildren;
	LocalAttachedChildren.Reserve(16);
	GetChildrenComponents(true, LocalAttachedChildren);
	for (int32 Idx = 0; Idx < LocalAttachedChildren.Num(); ++Idx)
	{
		if (!LocalAttachedChildren[Idx])
			continue;

		USceneComponent * pChild = LocalAttachedChildren[Idx];
		if (UStaticMeshComponent * StaticMeshComponent = Cast<UStaticMeshComponent>(pChild))
		{
			if (!StaticMeshComponent || StaticMeshComponent->IsPendingKill())
				continue;

			FBox StaticMeshBounds = StaticMeshComponent->Bounds.GetBox();
			if (StaticMeshBounds.IsValid)
				BoxBounds += StaticMeshBounds;
		}
	}

	//... all our Handles
	for (TMap< FString, UHoudiniHandleComponent * >::TConstIterator Iter(HandleComponents); Iter; ++Iter)
	{
		UHoudiniHandleComponent * HandleComponent = Iter.Value();
		if (!HandleComponent)
			continue;

		BoxBounds += HandleComponent->GetComponentLocation();
	}

	// ... all our curves
	for (TMap< FHoudiniGeoPartObject, UHoudiniSplineComponent* >::TConstIterator Iter(SplineComponents); Iter; ++Iter)
	{
		UHoudiniSplineComponent * SplineComponent = Iter.Value();
		if (!SplineComponent || !SplineComponent->IsValidLowLevel())
			continue;

		TArray<FVector> SplinePositions;
		SplineComponent->GetCurvePositions(SplinePositions);

		for (int32 n = 0; n < SplinePositions.Num(); n++)
		{
			BoxBounds += SplinePositions[n];
		}
	}

	// ... and inputs
	for (int32 n = 0; n < Inputs.Num(); n++)
	{
		UHoudiniAssetInput* CurrentInput = Inputs[n];
		if (!CurrentInput || CurrentInput->IsPendingKill())
			continue;

		if (CurrentInput == IgnoreInput)
			continue;

		FBox StaticMeshBounds = CurrentInput->GetInputBounds(GetComponentLocation());
		if (StaticMeshBounds.IsValid)
			BoxBounds += StaticMeshBounds;
	}

	// ... all our landscapes
	if (!bIgnoreGeneratedLandscape)
	{
		for (TMap< FHoudiniGeoPartObject, TWeakObjectPtr<ALandscapeProxy> >::TConstIterator Iter(LandscapeComponents); Iter; ++Iter)
		{
			ALandscapeProxy * Landscape = Iter.Value().Get();
			if (!Landscape)
				continue;

			FVector Origin, Extent;
			Landscape->GetActorBounds(false, Origin, Extent);

			FBox LandscapeBounds = FBox::BuildAABB(Origin, Extent);
			BoxBounds += LandscapeBounds;
		}
	}

	// If nothing was found, init with the asset's location
	if (BoxBounds.GetVolume() == 0.0f)
		BoxBounds += GetComponentLocation();
	*/

	return BoxBounds;
}

void
UHoudiniAssetComponent::ClearRefineMeshesTimer()
{
	UWorld *World = GetWorld();
	if (!World)
	{
		HOUDINI_LOG_ERROR(TEXT("Cannot ClearRefineMeshesTimer, World is nullptr!"));
		return;
	}
	
	World->GetTimerManager().ClearTimer(RefineMeshesTimer);
}

void
UHoudiniAssetComponent::SetRefineMeshesTimer()
{
	UWorld *World = GetWorld();
	if (!World)
	{
		HOUDINI_LOG_ERROR(TEXT("Cannot SetRefineMeshesTimer, World is nullptr!"));
		return;
	}

	// Check if timer-based proxy mesh refinement is enable for this component
	const bool bEnableTimer = IsProxyStaticMeshRefinementByTimerEnabled();
	const float TimeSeconds = GetProxyMeshAutoRefineTimeoutSeconds();
	if (bEnableTimer)
	{
		World->GetTimerManager().SetTimer(RefineMeshesTimer, this, &UHoudiniAssetComponent::OnRefineMeshesTimerFired, 1.0f, false, TimeSeconds);
	}
	else
	{
		World->GetTimerManager().ClearTimer(RefineMeshesTimer);
	}
}

void 
UHoudiniAssetComponent::OnRefineMeshesTimerFired()
{
	HOUDINI_LOG_MESSAGE(TEXT("UHoudiniAssetComponent::OnRefineMeshesTimerFired()"));
	if (OnRefineMeshesTimerDelegate.IsBound())
	{
		OnRefineMeshesTimerDelegate.Broadcast(this);
	}
}

bool
UHoudiniAssetComponent::HasAnyCurrentProxyOutput() const
{
	for (const UHoudiniOutput *Output : Outputs)
	{
		if (Output->HasAnyCurrentProxy())
		{
			return true;
		}
	}

	return false;
}

bool
UHoudiniAssetComponent::HasAnyProxyOutput() const
{
	for (const UHoudiniOutput *Output : Outputs)
	{
		if (Output->HasAnyProxy())
		{
			return true;
		}
	}

	return false;
}

bool
UHoudiniAssetComponent::HasAnyOutputComponent() const
{
	for (UHoudiniOutput *Output : Outputs)
	{
		for(auto& CurrentOutputObject : Output->GetOutputObjects())
		{
			if(CurrentOutputObject.Value.OutputComponent)
				return true;
		}
	}

	return false;
}

bool
UHoudiniAssetComponent::HasOutputObject(UObject* InOutputObjectToFind) const
{
	for (const auto& CurOutput : Outputs)
	{
		for (const auto& CurOutputObject : CurOutput->GetOutputObjects())
		{
			if (CurOutputObject.Value.OutputObject == InOutputObjectToFind)
				return true;
			else if (CurOutputObject.Value.OutputComponent == InOutputObjectToFind)
				return true;
			else if (CurOutputObject.Value.ProxyObject == InOutputObjectToFind)
				return true;
			else if (CurOutputObject.Value.ProxyComponent == InOutputObjectToFind)
				return true;
		}
	}

	return false;
}

bool
UHoudiniAssetComponent::IsHoudiniCookedDataAvailable(bool &bOutNeedsRebuildOrDelete, bool &bOutInvalidState) const
{
	// Get the state of the asset and check if it is pre-cook, cooked, pending delete/rebuild or invalid
	bOutNeedsRebuildOrDelete = false;
	bOutInvalidState = false;
	switch (AssetState)
	{
	case EHoudiniAssetState::NeedInstantiation:
	case EHoudiniAssetState::PreInstantiation:
	case EHoudiniAssetState::Instantiating:
	case EHoudiniAssetState::PreCook:
	case EHoudiniAssetState::Cooking:
	case EHoudiniAssetState::PostCook:
	case EHoudiniAssetState::PreProcess:
	case EHoudiniAssetState::Processing:
		return false;
		break;
	case EHoudiniAssetState::None:
		return true;
		break;
	case EHoudiniAssetState::NeedRebuild:
	case EHoudiniAssetState::NeedDelete:
	case EHoudiniAssetState::Deleting:
		bOutNeedsRebuildOrDelete = true;
		break;
	default:
		bOutInvalidState = true;
		break;
	}

	return false;
}

void
UHoudiniAssetComponent::SetInputPresets(const TMap<UObject*, int32>& InPresets)
{
	// Set the input preset for this HAC
#if WITH_EDITOR
	InputPresets = InPresets;
#endif
}


void
UHoudiniAssetComponent::ApplyInputPresets()
{
	if (InputPresets.Num() <= 0)
		return;

#if WITH_EDITOR
	// Ignore inputs that have been preset to curve
	TArray<UHoudiniInput*> InputArray;
	for (auto CurrentInput : Inputs)
	{
		if (!CurrentInput || CurrentInput->IsPendingKill())
			continue;

		if (CurrentInput->GetInputType() != EHoudiniInputType::Curve)
			InputArray.Add(CurrentInput);
	}

	// Try to apply the supplied Object to the Input
	for (TMap< UObject*, int32 >::TIterator IterToolPreset(InputPresets); IterToolPreset; ++IterToolPreset)
	{
		UObject * Object = IterToolPreset.Key();
		if (!Object || Object->IsPendingKill())
			continue;

		int32 InputNumber = IterToolPreset.Value();
		if (!InputArray.IsValidIndex(InputNumber))
			continue;

		// If the object is a landscape, add a new landscape input
		if (Object->IsA<ALandscapeProxy>())
		{
			// selecting a landscape 
			int32 InsertNum = InputArray[InputNumber]->GetNumberOfInputObjects(EHoudiniInputType::Landscape);
			if (InsertNum == 0)
			{
				// Landscape inputs only support one object!
				InputArray[InputNumber]->SetInputObjectAt(EHoudiniInputType::Landscape, InsertNum, Object);
			}
		}

		// If the object is an actor, add a new world input
		if (Object->IsA<AActor>())
		{
			// selecting an actor 
			int32 InsertNum = InputArray[InputNumber]->GetNumberOfInputObjects(EHoudiniInputType::World);
			InputArray[InputNumber]->SetInputObjectAt(EHoudiniInputType::World, InsertNum, Object);
		}

		// If the object is a static mesh, add a new geometry input (TODO: or BP ? )
		if (Object->IsA<UStaticMesh>())
		{
			// selecting a Staticn Mesh
			int32 InsertNum = InputArray[InputNumber]->GetNumberOfInputObjects(EHoudiniInputType::Geometry);
			InputArray[InputNumber]->SetInputObjectAt(EHoudiniInputType::Geometry, InsertNum, Object);
		}

		if (Object->IsA<AHoudiniAssetActor>())
		{
			// selecting a Houdini Asset 
			int32 InsertNum = InputArray[InputNumber]->GetNumberOfInputObjects(EHoudiniInputType::Asset);
			if (InsertNum == 0)
			{
				// Assert inputs only support one object!
				InputArray[InputNumber]->SetInputObjectAt(EHoudiniInputType::Asset, InsertNum, Object);
			}
		}
	}

	// The input objects have been set, now change the input type
	for (auto CurrentInput : Inputs)
	{		
		int32 NumGeo = CurrentInput->GetNumberOfInputObjects(EHoudiniInputType::Geometry);
		int32 NumAsset = CurrentInput->GetNumberOfInputObjects(EHoudiniInputType::Asset);
		int32 NumWorld = CurrentInput->GetNumberOfInputObjects(EHoudiniInputType::World);
		int32 NumLandscape = CurrentInput->GetNumberOfInputObjects(EHoudiniInputType::Landscape);

		EHoudiniInputType NewInputType = EHoudiniInputType::Invalid;
		if (NumLandscape > 0 && NumLandscape >= NumGeo && NumLandscape >= NumAsset && NumLandscape >= NumWorld)
			NewInputType = EHoudiniInputType::Landscape;
		else if (NumWorld > 0 && NumWorld >= NumGeo && NumWorld >= NumAsset && NumWorld >= NumLandscape)
			NewInputType = EHoudiniInputType::World;
		else if (NumAsset > 0 && NumAsset >= NumGeo && NumAsset >= NumWorld && NumAsset >= NumLandscape)
			NewInputType = EHoudiniInputType::Asset;
		else if (NumGeo > 0 && NumGeo >= NumAsset && NumGeo >= NumWorld && NumGeo >= NumLandscape)
			NewInputType = EHoudiniInputType::Geometry;

		if (NewInputType == EHoudiniInputType::Invalid)
			continue;

		// Change the input type, unless if it was preset to a different type and we have object for the preset type
		if (CurrentInput->GetInputType() == EHoudiniInputType::Geometry && NewInputType != EHoudiniInputType::Geometry)
		{
			CurrentInput->SetInputType(NewInputType);
		}
		else
		{
			// Input type was preset, only change if that type is empty
			if(CurrentInput->GetNumberOfInputObjects() <= 0)
				CurrentInput->SetInputType(NewInputType);
		}
	}
#endif

	// Discard the tool presets after their first setup
	InputPresets.Empty();
}


bool
UHoudiniAssetComponent::IsComponentValid() const
{
	if (!IsValidLowLevel())
		return false;

	if (IsTemplate())
		return false;

	if (IsPendingKillOrUnreachable())
		return false;

	if (!GetOuter()) //|| !GetOuter()->GetLevel() )
		return false;

	return true;
}

bool
UHoudiniAssetComponent::IsInstantiatingOrCooking() const
{
	return HapiGUID.IsValid();
}