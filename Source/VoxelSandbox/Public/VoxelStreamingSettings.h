// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "VoxelStreamingSettings.generated.h"

/**
 * Tunables for loading chunks around the player. Distances are in chunks and measured as a circle around the
 * player's chunk. Cube data is loaded one ring further out than meshes are built, so every meshed chunk has all
 * four side neighbours and its border faces are culled correctly the first time.
 */
USTRUCT(BlueprintType)
struct VOXELSANDBOX_API FVoxelStreamingSettings
{
	GENERATED_BODY()

	/** Chunks within this distance of the player's chunk get a mesh. Data is loaded one chunk further. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streaming", meta = (ClampMin = "1", ToolTip = "Chunks within this distance (in chunks, a circle around the player's chunk) are drawn. Cube data is loaded one chunk further so edges cull correctly. 8 = about 128 cubes."))
	int32 ViewDistanceChunks = 8;

	/** Radius loaded and drawn before the player spawns, so they never fall through. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streaming", meta = (ClampMin = "1", ToolTip = "Chunks loaded and drawn at once before the player spawns (a circle around chunk 0, 0). The rest streams in over the next frames. Higher = longer start, more ground ready."))
	int32 StartupRadiusChunks = 3;

	/** How often the player's chunk is checked, in seconds. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streaming", meta = (ClampMin = "0.05", ToolTip = "Seconds between checks of which chunk the player is in. Work only starts when that chunk changes, so this can be slow."))
	float PlayerCheckInterval = 0.25f;

	/** Most chunks generated (spawned, filled, caves, saved edits) in one frame. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streaming", meta = (ClampMin = "1", ToolTip = "Most new chunks generated per frame (noise, fill, caves, saved edits). Higher = the world fills faster but each frame costs more."))
	int32 MaxChunksLoadedPerStep = 2;

	/** Most chunk meshes built in one frame. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streaming", meta = (ClampMin = "1", ToolTip = "Most chunk meshes (and their collision) built per frame. Meshing is the most expensive step, so keep this low."))
	int32 MaxMeshBuildsPerStep = 2;

	/** Fog starts this many chunks before the view edge and is opaque by the edge. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streaming", meta = (ClampMin = "0", ToolTip = "The fog's Start Distance is set to (ViewDistanceChunks − this) chunks, so new chunks appear inside the fog. Tune the fog's density on the Exponential Height Fog actor so it is opaque by the view edge."))
	int32 FogFadeChunks = 3;

	/** Chunks further than this from the player's chunk are destroyed. Kept above ViewDistanceChunks + 1. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streaming", meta = (ClampMin = "2", ToolTip = "Chunks further than this (in chunks) are destroyed; their saved edits stay in the world's Changes map. Must be at least ViewDistanceChunks + 1 (the data ring), and a bit more avoids chunks loading and unloading as you step back and forth over the edge. The streaming step uses max(this, ViewDistanceChunks + 1)."))
	int32 UnloadDistanceChunks = 10;

	/** Most chunks unloaded, and most meshes cleared, in one frame. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streaming", meta = (ClampMin = "1", ToolTip = "Most chunks destroyed, and most meshes cleared (chunks that left ViewDistanceChunks), per frame. Both are cheap, so this can be higher than the load and mesh budgets."))
	int32 MaxChunksUnloadedPerStep = 4;

	/** Most worm regions planned ahead in one frame. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Streaming", meta = (ClampMin = "1", ToolTip = "Most worm regions planned per frame, ahead of the chunks that need them, so their cost is spread out instead of landing on the frame a chunk loads. One region is a few milliseconds."))
	int32 MaxWormRegionsPlannedPerStep = 1;
};
