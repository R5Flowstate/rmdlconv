#pragma once

// Oodle is loaded from an oo2core DLL at runtime (OODLE_DLL, else
// oo2core_9..5_win64.dll next to the exe); nothing is vendored or linked.
namespace Oodle
{
	bool Decompress(const uint8_t* const src, const size_t compLen, uint8_t* const dst, const size_t rawLen);
}
