#include <pch.h>
#include <core/oodle.h>

typedef void* HMODULE;
extern "C" __declspec(dllimport) HMODULE __stdcall LoadLibraryA(const char*);
extern "C" __declspec(dllimport) void* __stdcall GetProcAddress(HMODULE, const char*);

namespace
{
	typedef intptr_t OO_SINTa;
	typedef OO_SINTa(*pfnDecompress)(const void*, OO_SINTa, void*, OO_SINTa, int, int, int, void*, OO_SINTa, void*, void*, void*, OO_SINTa, int);

	const char* const s_dllNames[] = {
		"oo2core_9_win64.dll",
		"oo2core_8_win64.dll",
		"oo2core_7_win64.dll",
		"oo2core_6_win64.dll",
		"oo2core_5_win64.dll",
	};

	pfnDecompress GetDecompress()
	{
		static pfnDecompress fn = nullptr;
		static bool tried = false;

		if (tried)
			return fn;

		tried = true;

		HMODULE hMod = nullptr;
		char* envName = nullptr;
		size_t envLen = 0;
		_dupenv_s(&envName, &envLen, "OODLE_DLL");

		if (envName && *envName)
		{
			hMod = LoadLibraryA(envName);

			if (!hMod)
				printf("[!] OODLE_DLL is set to \"%s\" but it could not be loaded.\n", envName);
		}

		free(envName);

		for (size_t i = 0; !hMod && i < sizeof(s_dllNames) / sizeof(s_dllNames[0]); i++)
			hMod = LoadLibraryA(s_dllNames[i]);

		if (!hMod)
		{
			printf("[!] No oo2core DLL found; compressed vertex groups cannot be unpacked. Set OODLE_DLL to one, or place it next to rmdlconv.exe.\n");
			return fn;
		}

		fn = reinterpret_cast<pfnDecompress>(GetProcAddress(hMod, "OodleLZ_Decompress"));

		if (!fn)
			printf("[!] An oo2core DLL loaded but does not export OodleLZ_Decompress.\n");

		return fn;
	}
}

bool Oodle::Decompress(const uint8_t* const src, const size_t compLen, uint8_t* const dst, const size_t rawLen)
{
	const pfnDecompress fn = GetDecompress();

	if (!fn)
		return false;

	const OO_SINTa result = fn(
		src, static_cast<OO_SINTa>(compLen),
		dst, static_cast<OO_SINTa>(rawLen),
		1,  // OodleLZ_FuzzSafe_Yes
		0,  // OodleLZ_CheckCRC_No
		0,  // OodleLZ_Verbosity_None
		nullptr, 0, nullptr, nullptr, nullptr, 0,
		3); // OodleLZ_Decode_Unthreaded

	return static_cast<size_t>(result) == rawLen;
}
