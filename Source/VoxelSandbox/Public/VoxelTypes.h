// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

/**
 * Shared voxel constants and lookup tables used by the C++ chunk core.
 * Everything here mirrors a Blueprint asset or variable; keep them in sync.
 */
namespace Voxel
{
	/**
	 * Cube type byte values. MUST stay in sync with the E_CubeType enum
	 * (Content/Voxel/Data/E_CubeType): Air, Rock, Grass, Snow in that order.
	 */
	namespace CubeType
	{
		inline constexpr uint8 Air   = 0;
		inline constexpr uint8 Rock  = 1;
		inline constexpr uint8 Grass = 2;
		inline constexpr uint8 Snow  = 3;
	}

	/** Cubes along X and Y in one chunk. Must match DA_VoxelSettings.ChunkSize (16). */
	inline constexpr int32 ChunkSize = 16;

	/** Columns in one chunk (ChunkSize × ChunkSize); one entry per column in the column arrays. */
	inline constexpr int32 ColumnCount = ChunkSize * ChunkSize;

	/** Corners per face quad. */
	inline constexpr int32 CornersPerFace = 4;

	/** Index-buffer entries per face quad (two triangles). */
	inline constexpr int32 IndicesPerFace = 6;

	/** Number of cube faces / neighbour directions. */
	inline constexpr int32 FaceCount = 6;

	/** One cube face: outward normal and its 4 corners in cube units (0 or 1). */
	struct FFaceDef
	{
		int32 Normal[3];
		int32 Corners[CornersPerFace][3];
	};

	/**
	 * Face table, copied from BP_VoxelChunk.FaceTable (same order: +X, −X, +Y, −Y, +Z, −Z,
	 * same corner order) so the vertex order matches the Blueprint mesh exactly.
	 */
	inline constexpr FFaceDef FaceTable[FaceCount] =
	{
		{ {  1,  0,  0 }, { { 1, 0, 0 }, { 1, 1, 0 }, { 1, 1, 1 }, { 1, 0, 1 } } },
		{ { -1,  0,  0 }, { { 0, 1, 0 }, { 0, 0, 0 }, { 0, 0, 1 }, { 0, 1, 1 } } },
		{ {  0,  1,  0 }, { { 1, 1, 0 }, { 0, 1, 0 }, { 0, 1, 1 }, { 1, 1, 1 } } },
		{ {  0, -1,  0 }, { { 0, 0, 0 }, { 1, 0, 0 }, { 1, 0, 1 }, { 0, 0, 1 } } },
		{ {  0,  0,  1 }, { { 0, 0, 1 }, { 1, 0, 1 }, { 1, 1, 1 }, { 0, 1, 1 } } },
		{ {  0,  0, -1 }, { { 0, 1, 0 }, { 1, 1, 0 }, { 1, 0, 0 }, { 0, 0, 0 } } },
	};

	/** Per-corner UVs, copied from BP_VoxelChunk.FaceUVs. */
	inline constexpr float FaceUVs[CornersPerFace][2] =
	{
		{ 0.f, 0.f }, { 1.f, 0.f }, { 1.f, 1.f }, { 0.f, 1.f },
	};

	/** Two triangles per quad as corner indices, copied from BP_VoxelChunk.QuadWinding. */
	inline constexpr int32 QuadWinding[IndicesPerFace] = { 0, 2, 1, 0, 3, 2 };
}
