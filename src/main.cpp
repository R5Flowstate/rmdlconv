// Copyright (c) 2022, rexx
// See LICENSE.txt for licensing information (GPL v3)

#include <pch.h>
#include <algorithm>
#include <core/CommandLine.h>
#include <studio/versions.h>
#include <core/utils.h>
#include <collision/collision_smd.h>

const char* pszVersionHelpString = {
	"Please input the version of your model:\n"
	"-- OLD --\n"
	"8:    s0,1\n"
	"9:    s2\n"
	"10:   s3,4\n"
	"11:   s5\n"
	"12:   s6\n"
	"-- NEW --\n"
	"12.1: s7,8\n"
	"12.2: s9,10,11\n"
	"13:   s12\n"
	"14:   s13.1\n"
	"14.1: s14\n"
	"15:   s15\n"
	"16:   s16,17\n"
	"17:   s18\n"
	"18:   s18.1\n"
	"19:   s19\n"
	"19.1: s19.1+ (Season 19+)\n"
	"> "
};

const char* pszRSeqVersionHelpString = {
	"Please input the version of your sequence : \n"
	"7:    s0,1,3,4,5,6\n"
	"7.1:  s7,8\n"
	"10:   s9,10,11,12,13,14\n"
	"11:   s15\n"
	"> "
};

// move on from this
void LegacyConversionHandling(CommandLine& cmdline)
{
	// using command args
	if (cmdline.argc > 2)
		return;

	if (!FILE_EXISTS(cmdline.argv[1]))
		Error("couldn't find input file\n");

	std::string mdlPath(cmdline.argv[1]);

	BinaryIO mdlIn;
	mdlIn.open(mdlPath, BinaryIOMode::Read);

	if (mdlIn.read<int>() == 'TSDI')
	{
		int mdlVersion = mdlIn.read<int>();

		switch (mdlVersion)
		{
		case MdlVersion::GARRYSMOD:
		{
			uintmax_t mdlFileSize = GetFileSize(mdlPath);

			mdlIn.seek(0, std::ios::beg);

			char* mdlBuf = new char[mdlFileSize];

			mdlIn.getReader()->read(mdlBuf, mdlFileSize);

			ConvertMDL48To54(mdlBuf, mdlPath, mdlPath);

			delete[] mdlBuf;

			break;
		}
		case MdlVersion::PORTAL2:
		{
			uintmax_t mdlFileSize = GetFileSize(mdlPath);

			mdlIn.seek(0, std::ios::beg);

			char* mdlBuf = new char[mdlFileSize];

			mdlIn.getReader()->read(mdlBuf, mdlFileSize);

			ConvertMDL49To54(mdlBuf, mdlPath, mdlPath);

			delete[] mdlBuf;

			break;
		}
		case MdlVersion::TITANFALL:
		{
			uintmax_t mdlFileSize = GetFileSize(mdlPath);

			mdlIn.seek(0, std::ios::beg);

			char* mdlBuf = new char[mdlFileSize];

			mdlIn.getReader()->read(mdlBuf, mdlFileSize);

			ConvertMDL52To53(mdlBuf, mdlPath, mdlPath);

			delete[] mdlBuf;

			break;
		}
		case MdlVersion::TITANFALL2:
		{
			uintmax_t mdlFileSize = GetFileSize(mdlPath);

			mdlIn.seek(0, std::ios::beg);

			char* mdlBuf = new char[mdlFileSize];

			mdlIn.getReader()->read(mdlBuf, mdlFileSize);

			ConvertMDL53To54(mdlBuf, mdlPath, mdlPath);

			delete[] mdlBuf;

			break;
		}
		case MdlVersion::APEXLEGENDS:
		{
			// rmdl subversion
			std::string version = "12.1";

			if (cmdline.HasParam("-version"))
			{
				version = cmdline.GetParamValue("-version", "12.1");
			}
			else
			{
				std::cout << pszVersionHelpString;
				std::cin >> version;
			}

			printf("Input file is RMDL v%s. attempting conversion...\n", version.c_str());

			if (version == "12.1") // handle 12.1 model conversions
			{
				// Create output folder next to input file
				std::filesystem::path inputPath(mdlPath);
				std::filesystem::path outputDir = inputPath.parent_path() / "rmdlconv_out";
				std::filesystem::create_directories(outputDir);

				// convert v12.1 rmdl to v10 rmdl
				uintmax_t mdlFileSize = GetFileSize(mdlPath);
				mdlIn.seek(0, std::ios::beg);
				char* mdlBuf = new char[mdlFileSize];
				mdlIn.getReader()->read(mdlBuf, mdlFileSize);

				std::string rmdlOutputPath = (outputDir / inputPath.filename()).string();
				ConvertRMDL121To10(mdlBuf, mdlPath, rmdlOutputPath);

				delete[] mdlBuf;

				// convert v12.1 vg to v9 vg
				std::string vgFilePath = ChangeExtension(mdlPath, "vg");

				if (FILE_EXISTS(vgFilePath))
				{
					uintmax_t vgInputSize = GetFileSize(vgFilePath);

					char* vgInputBuf = new char[vgInputSize];

					std::ifstream ifs(vgFilePath, std::ios::in | std::ios::binary);

					ifs.read(vgInputBuf, vgInputSize);

					// if 0tVG magic - output to rmdlconv_out/ folder with original filename
					if (*(int*)vgInputBuf == 'GVt0')
					{
						std::string vgOutputPath = (outputDir / std::filesystem::path(vgFilePath).filename()).string();
						printf("VG Output: %s\n", vgOutputPath.c_str());
						ConvertVGData_12_1(vgInputBuf, vgFilePath, vgOutputPath);
					}
					else
						delete[] vgInputBuf;
				}

				break;
			}
			else if (version == "12.2") // handle 12.2 model conversions (Season 9-11)
			{
				// Create output folder next to input file
				std::filesystem::path inputPath(mdlPath);
				std::filesystem::path outputDir = inputPath.parent_path() / "rmdlconv_out";
				std::filesystem::create_directories(outputDir);

				// convert v12.2 rmdl to v10 rmdl
				uintmax_t mdlFileSize = GetFileSize(mdlPath);
				mdlIn.seek(0, std::ios::beg);
				char* mdlBuf = new char[mdlFileSize];
				mdlIn.getReader()->read(mdlBuf, mdlFileSize);

				std::string rmdlOutputPath = (outputDir / inputPath.filename()).string();
				ConvertRMDL122To10(mdlBuf, mdlPath, rmdlOutputPath);

				delete[] mdlBuf;

				break;
			}
			else if (version == "8")
			{
				intmax_t mdlFileSize = GetFileSize(mdlPath);

				mdlIn.seek(0, std::ios::beg);

				char* mdlBuf = new char[mdlFileSize];

				mdlIn.getReader()->read(mdlBuf, mdlFileSize);

				ConvertRMDL8To10(mdlBuf, mdlPath, mdlPath);

				delete[] mdlBuf;

				break;
			}
			else if (version == "16" || version == "17" || version == "18" || version == "19")
			{
				// v16/v17/v18/v19 (Season 13-19) conversion
				uintmax_t mdlFileSize = GetFileSize(mdlPath);

				mdlIn.seek(0, std::ios::beg);

				char* mdlBuf = new char[mdlFileSize];

				mdlIn.getReader()->read(mdlBuf, mdlFileSize);

				int subver = std::stoi(version);
				ConvertRMDL160To10(mdlBuf, mdlFileSize, mdlPath, mdlPath, subver);

				delete[] mdlBuf;

				break;
			}
			else if (version == "19.1" || version == "19")
			{
				// v19.1 (Season 19+) conversion
				uintmax_t mdlFileSize = GetFileSize(mdlPath);

				mdlIn.seek(0, std::ios::beg);

				char* mdlBuf = new char[mdlFileSize];

				mdlIn.getReader()->read(mdlBuf, mdlFileSize);

				ConvertRMDL191To10(mdlBuf, mdlFileSize, mdlPath, mdlPath);

				delete[] mdlBuf;

				break;
			}
			else
			{
				Error("version is not currently supported\n");
			}

			break;
		}
		default:
		{
			Error("MDL version %i is currently unsupported\n", mdlVersion);
			break;
		}
		}
	}
	else if (mdlPath.find(".rseq") != std::string::npos)
	{
		// Handle .rseq sequence files
		printf("seq gaming\n");

		std::string version = "7.1";

		if (cmdline.HasParam("-version"))
		{
			version = cmdline.GetParamValue("-version", "7.1");
		}
		else
		{
			std::cout << pszRSeqVersionHelpString;
			std::cin >> version;
		}

		uintmax_t seqFileSize = GetFileSize(mdlPath);

		mdlIn.seek(0, std::ios::beg);

		char* seqBuf = new char[seqFileSize];

		mdlIn.getReader()->read(seqBuf, seqFileSize);


		std::string rseqExtPath = ChangeExtension(mdlPath, "rseq_ext");
		char* seqExternalBuf = nullptr;
		if (FILE_EXISTS(rseqExtPath))
		{
			int seqExtFileSize = GetFileSize(rseqExtPath);

			seqExternalBuf = new char[seqExtFileSize];

			std::ifstream ifs(rseqExtPath, std::ios::in | std::ios::binary);

			ifs.read(seqExternalBuf, seqExtFileSize);
		}

		if (version == "7.1")
		{
			//printf("converting rseq version 7.1 to version 7\n");

			ConvertRSEQFrom71To7(seqBuf, seqExternalBuf, mdlPath);
		}
		else if (version == "10")
		{
			ConvertRSEQFrom10To7(seqBuf, seqExternalBuf, mdlPath);
		}

		delete[] seqBuf;
	}
	else if (mdlPath.find(".rmdl") != std::string::npos)
	{
		// Handle .rmdl files without 'TSDI' magic (v16+ uses different header structure)
		printf("RMDL file detected without standard magic - likely v16+ format\n");

		std::string version = "16";

		if (cmdline.HasParam("-version"))
		{
			version = cmdline.GetParamValue("-version", "16");
		}
		else
		{
			std::cout << pszVersionHelpString;
			std::cin >> version;
		}

		if (version == "16" || version == "17" || version == "18" || version == "19")
		{
			uintmax_t mdlFileSize = GetFileSize(mdlPath);

			mdlIn.seek(0, std::ios::beg);

			char* mdlBuf = new char[mdlFileSize];

			mdlIn.getReader()->read(mdlBuf, mdlFileSize);

			int subver = std::stoi(version);
			ConvertRMDL160To10(mdlBuf, mdlFileSize, mdlPath, mdlPath, subver);

			delete[] mdlBuf;
		}
		else if (version == "19.1")
		{
			uintmax_t mdlFileSize = GetFileSize(mdlPath);

			mdlIn.seek(0, std::ios::beg);

			char* mdlBuf = new char[mdlFileSize];

			mdlIn.getReader()->read(mdlBuf, mdlFileSize);

			ConvertRMDL191To10(mdlBuf, mdlFileSize, mdlPath, mdlPath);

			delete[] mdlBuf;
		}
		else
		{
			Error("For RMDL files without standard magic, only v16+ conversion is currently supported\n");
		}
	}
	else
	{
		Error("invalid input file. must be a valid .(r)mdl or .rseq file\n");
	}
}

// Help text for batch mode
const char* pszBatchHelpString = {
	"Batch conversion mode:\n"
	"  rmdlconv.exe -v<version> <input_folder> [output_folder]\n"
	"\n"
	"Version flags:\n"
	"  -v8     Model v8 (S0-S6 / S3-legacy); add -targetversion 17 for S21 client\n"
	"  -v49    Portal 2 MDL v49; requires -targetversion 17 for S21 client\n"
	"  -vp2    alias for -v49\n"
	"  -v121   Model v12.1 (S7-8)\n"
	"  -v122   Model v12.2 (S9-11); add -targetversion 17 for S21 client\n"
	"  -v123   Model v12.3 (transition)\n"
	"  -v124   Model v12.4 (transition)\n"
	"  -v125   Model v12.5/v13 (S12)\n"
	"  -v13    Model v13 (S12) - alias for v12.5\n"
	"  -v131   Model v13.1 (transition)\n"
	"  -v14    Model v14 (S13)\n"
	"  -v141   Model v14.1 (S14)\n"
	"  -v15    Model v15 (S15)\n"
	"  -v16    Model v16 (S16-17, rseq v11)\n"
	"  -v17    Model v17 (S17-18, rseq v11)\n"
	"  -v18    Model v18 (S18, rseq v12)\n"
	"  -v19    Model v19 (S19, rseq v12)\n"
	"  -v191   Model v19.1 (S19+, rseq v12.1)\n"
	"\n"
	"If output_folder is not specified, uses '<input_folder>_rmdlconv_out'\n"
	"Internal folder structure is preserved.\n"
	"\n"
	"Example:\n"
	"  rmdlconv.exe -v122 -targetversion 17 C:\\models\\s11 C:\\models\\s21\n"
	"  rmdlconv.exe -v8 -targetversion 17 C:\\models\\s3 C:\\models\\s21\n"
	"  rmdlconv.exe -v49 -targetversion 17 C:\\models\\p2 C:\\models\\s21\n"
	"  rmdlconv.exe -v191 C:\\retail_models\n"
};

// Batch conversion function
// Shared S10 (mdl_ v12.2) -> S21 CLIENT (v17) single-model conversion: the sibling .vg
// (rev2 0tVG -> rev4, the genuine-S21-matching layout) is converted FIRST so its produced size
// feeds the rmdl group header, then the v17 rmdl, then the .phy (20B IVPS -> 4B compact). Used by
// BOTH the single `-convertmodel` path and the `-v122 -targetversion 17` batch path -- one
// implementation, no divergence.
void ConvertClientModel_122To17(const std::string& inputFile, const std::string& outputFile)
{
	uintmax_t fileSize = GetFileSize(inputFile);
	std::unique_ptr<char[]> pMDL(new char[fileSize]);
	{
		std::ifstream ifs(inputFile, std::ios::in | std::ios::binary);
		ifs.read(pMDL.get(), fileSize);
		ifs.close();
	}
	std::filesystem::create_directories(std::filesystem::path(outputFile).parent_path());

	// VG first (the rmdl group header must describe the produced rev4 VG).
	uint32_t vgDecompSize = 0;
	const std::string vgIn = ChangeExtension(inputFile, "vg");
	const std::string vgOut = ChangeExtension(outputFile, "vg");
	if (FILE_EXISTS(vgIn))
	{
		uintmax_t vgSize = GetFileSize(vgIn);
		std::unique_ptr<char[]> vgBuf(new char[vgSize]);
		std::ifstream vgIfs(vgIn, std::ios::in | std::ios::binary);
		vgIfs.read(vgBuf.get(), vgSize);
		vgIfs.close();

		vgDecompSize = static_cast<uint32_t>(ConvertVGData_Rev2To17(vgBuf.get(), vgSize, vgIn, vgOut));

		// Static props consult a PERMANENT vertex cache (.vg_static -> pStaticPropVtxCache);
		// emit a copy. Skinned/dynamic models stream the .vg and ignore it.
		if (vgDecompSize > 0)
		{
			std::error_code ec;
			std::filesystem::copy_file(vgOut, ChangeExtension(outputFile, "vg_static"),
				std::filesystem::copy_options::overwrite_existing, ec);
			if (ec)
				printf("[v17] WARNING: could not write .vg_static (%s)\n", ec.message().c_str());
		}
	}

	ConvertRMDL122To17(pMDL.get(), fileSize, inputFile, outputFile, vgDecompSize);

	// Sibling .phy (20B IVPS) -> 4B compact.
	const std::string phyIn = ChangeExtension(inputFile, "phy");
	if (FILE_EXISTS(phyIn))
	{
		uintmax_t phySize = GetFileSize(phyIn);
		std::unique_ptr<char[]> phyBuf(new char[phySize]);
		std::ifstream phyIfs(phyIn, std::ios::in | std::ios::binary);
		phyIfs.read(phyBuf.get(), phySize);
		phyIfs.close();

		ConvertPhy_122To17(phyBuf.get(), phySize, phyIn, ChangeExtension(outputFile, "phy"));
	}
}

// targetVersion: 0 = the version-default batch (e.g. -v122 -> DEDI v10); pass 17 with -v122 to
// route the S10 -> S21 CLIENT v17 batch (VG rev4 + v17 rmdl + phy) instead.
void BatchConvertModels(const std::string& sourceVersion, const std::string& inputFolder, const std::string& outputFolder, int targetVersion = 0)
{
	std::filesystem::path inputPath(inputFolder);
	std::filesystem::path outputPath(outputFolder);

	if (!std::filesystem::exists(inputPath))
		Error("Input folder does not exist: %s\n", inputFolder.c_str());

	if (!std::filesystem::is_directory(inputPath))
		Error("Input path is not a folder: %s\n", inputFolder.c_str());

	// Create output folder
	std::filesystem::create_directories(outputPath);

	printf("Batch converting from: %s\n", inputFolder.c_str());
	printf("Output folder: %s\n", outputFolder.c_str());
	printf("Source version: %s\n", sourceVersion.c_str());
	printf("\n");

	int successCount = 0;
	int failCount = 0;
	int totalCount = 0;

	// Recursively find all .rmdl files
	for (const auto& entry : std::filesystem::recursive_directory_iterator(inputPath))
	{
		if (!entry.is_regular_file())
			continue;

		std::string ext = entry.path().extension().string();
		std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

		// Portal 2 batch accepts .mdl; everything else is .rmdl.
		const bool wantMdl = (sourceVersion == "49" || sourceVersion == "p2" || sourceVersion == "portal2");
		if (wantMdl)
		{
			if (ext != ".mdl")
				continue;
		}
		else if (ext != ".rmdl")
		{
			continue;
		}

		totalCount++;

		// Calculate relative path from input folder
		std::filesystem::path relativePath = std::filesystem::relative(entry.path(), inputPath);
		std::filesystem::path outputFilePath = outputPath / relativePath;
		// Portal 2: emit .rmdl even when source was .mdl
		if (wantMdl && targetVersion == 17)
			outputFilePath.replace_extension(".rmdl");

		// Create output subdirectory if needed
		std::filesystem::create_directories(outputFilePath.parent_path());

		std::string inputFile = entry.path().string();
		std::string outputFile = outputFilePath.string();

		printf("[%d] Converting: %s\n", totalCount, relativePath.string().c_str());

		// S10 v12.2 -> S21 CLIENT v17 batch (VG rev4 + v17 rmdl + phy). `-v122` defaults to the
		// DEDI v10 path below; `-v122 -targetversion 17` routes here. One shared implementation.
		if ((sourceVersion == "12.2" || sourceVersion == "122") && targetVersion == 17)
		{
			try { ConvertClientModel_122To17(inputFile, outputFile); successCount++; }
			catch (const std::exception& e) { printf("  ERROR: %s\n", e.what()); failCount++; }
			continue;
		}

		// S3-legacy v8 -> S21 CLIENT v17. `-v8` defaults to VG-only upgrade (To10); `-v8 -targetversion 17`
		// does the full rmdl rebuild + rev1 VG -> rev4.
		if (sourceVersion == "8" && targetVersion == 17)
		{
			try { ConvertClientModel_8To17(inputFile, outputFile); successCount++; }
			catch (const std::exception& e) { printf("  ERROR: %s\n", e.what()); failCount++; }
			continue;
		}

		// Portal 2 (MDL v49) -> S21 CLIENT v17 via v8 intermediate.
		if ((sourceVersion == "49" || sourceVersion == "p2" || sourceVersion == "portal2") && targetVersion == 17)
		{
			try { ConvertClientModel_49To17(inputFile, outputFile); successCount++; }
			catch (const std::exception& e) { printf("  ERROR: %s\n", e.what()); failCount++; }
			continue;
		}

		// v19.1 -> S21 CLIENT v17. `-v191` defaults to the DEDI v10 path below;
		// `-v191 -targetversion 17` routes here, matching the single-file dispatch.
		// Without this the batch silently emits DEDI models for a client port, and a
		// port that skips the step ships raw v19.1: identical to v17 everywhere except
		// the bone array (16B vs 128B) and the seq/anim descs, so static props render
		// fine and every animated model gets a garbage pose.
		if ((sourceVersion == "19.1" || sourceVersion == "191") && targetVersion == 17)
		{
			try
			{
				uintmax_t fileSize = std::filesystem::file_size(entry.path());
				std::unique_ptr<char[]> pMDL(new char[fileSize]);
				std::ifstream ifs(inputFile, std::ios::in | std::ios::binary);
				if (!ifs.is_open())
				{
					printf("  ERROR: Could not open file\n");
					failCount++;
					continue;
				}
				ifs.read(pMDL.get(), fileSize);
				ifs.close();
				ConvertRMDL191To17(pMDL.get(), fileSize, inputFile, outputFile);
				successCount++;
			}
			catch (const std::exception& e) { printf("  ERROR: %s\n", e.what()); failCount++; }
			continue;
		}

		try
		{
			uintmax_t fileSize = std::filesystem::file_size(entry.path());
			std::unique_ptr<char[]> pMDL(new char[fileSize]);

			std::ifstream ifs(inputFile, std::ios::in | std::ios::binary);
			if (!ifs.is_open())
			{
				printf("  ERROR: Could not open file\n");
				failCount++;
				continue;
			}
			ifs.read(pMDL.get(), fileSize);
			ifs.close();

			if (sourceVersion == "8")
			{
				ConvertRMDL8To10(pMDL.get(), inputFile, outputFile);
			}
			else if (sourceVersion == "12.1" || sourceVersion == "121")
			{
				ConvertRMDL121To10(pMDL.get(), inputFile, outputFile);
			}
			else if (sourceVersion == "12.2" || sourceVersion == "122")
			{
				ConvertRMDL122To10(pMDL.get(), inputFile, outputFile);
			}
			else if (sourceVersion == "12.3" || sourceVersion == "123")
			{
				// v12.3 has identical studiohdr_t to v12.2 (only animation format changed)
				ConvertRMDL122To10(pMDL.get(), inputFile, outputFile);
			}
			else if (sourceVersion == "12.4" || sourceVersion == "124")
			{
				ConvertRMDL124To10(pMDL.get(), inputFile, outputFile);
			}
			else if (sourceVersion == "12.5" || sourceVersion == "125" || sourceVersion == "13")
			{
				ConvertRMDL125To10(pMDL.get(), inputFile, outputFile);
			}
			else if (sourceVersion == "13.1" || sourceVersion == "131")
			{
				// v13.1 has same studiohdr_t as v12.5, just adds uv3index to mstudiomodel_t
				ConvertRMDL125To10(pMDL.get(), inputFile, outputFile);
			}
			else if (sourceVersion == "14")
			{
				ConvertRMDL140To10(pMDL.get(), inputFile, outputFile);
			}
			else if (sourceVersion == "14.1" || sourceVersion == "141")
			{
				ConvertRMDL140To10(pMDL.get(), inputFile, outputFile);
			}
			else if (sourceVersion == "15")
			{
				ConvertRMDL150To10(pMDL.get(), inputFile, outputFile);
			}
			else if (sourceVersion == "16" || sourceVersion == "17" || sourceVersion == "18" || sourceVersion == "19")
			{
				int subver = std::stoi(sourceVersion);
				ConvertRMDL160To10(pMDL.get(), fileSize, inputFile, outputFile, subver);
			}
			else if (sourceVersion == "19.1" || sourceVersion == "191")
			{
				ConvertRMDL191To10(pMDL.get(), fileSize, inputFile, outputFile);
			}
			else
			{
				printf("  ERROR: Unknown source version\n");
				failCount++;
				continue;
			}

			// Also convert VG file if it exists (for v12.x and v13.x)
			// v12.1-13.1 use VG rev2 format (ConvertVGData_12_1)
			if (sourceVersion == "12.1" || sourceVersion == "121" ||
				sourceVersion == "12.2" || sourceVersion == "122" ||
				sourceVersion == "12.3" || sourceVersion == "123" ||
				sourceVersion == "12.4" || sourceVersion == "124" ||
				sourceVersion == "12.5" || sourceVersion == "125" || sourceVersion == "13" ||
				sourceVersion == "13.1" || sourceVersion == "131")
			{
				std::string vgInputFile = inputFile.substr(0, inputFile.length() - 5) + ".vg";
				if (std::filesystem::exists(vgInputFile))
				{
					uintmax_t vgSize = std::filesystem::file_size(vgInputFile);
					std::unique_ptr<char[]> vgBuf(new char[vgSize]);

					std::ifstream vgIfs(vgInputFile, std::ios::in | std::ios::binary);
					vgIfs.read(vgBuf.get(), vgSize);
					vgIfs.close();

					if (*(int*)vgBuf.get() == 'GVt0')
					{
						std::string vgOutputFile = outputFile.substr(0, outputFile.length() - 5) + ".vg";
						ConvertVGData_12_1(vgBuf.get(), vgInputFile, vgOutputFile);
						printf("  VG converted: %s\n", std::filesystem::path(vgOutputFile).filename().string().c_str());
					}
				}
			}

			successCount++;
		}
		catch (const std::exception& e)
		{
			printf("  ERROR: %s\n", e.what());
			failCount++;
		}
	}

	printf("\n");
	printf("========================================\n");
	printf("Batch conversion complete!\n");
	printf("  Total:   %d\n", totalCount);
	printf("  Success: %d\n", successCount);
	printf("  Failed:  %d\n", failCount);
	printf("========================================\n");
}

// Opt-in: when true, the v160 -> v8 conversion auto-generates a BVH4 from
// the companion .vg mesh for static props whose v160 source has bvhOffset==0.
// Default OFF -- most such props are intentionally collision-less in Apex
// (decals, foliage, decorative geometry) and giving them collision is wrong.
// Enable with CLI flag `-autogenbvh` only when you want to opt into the trade-off.
bool g_enableAutoGenBVH = false;

// Opt-in -phy_ivp: re-encode id=1 geoms as Valve/IVP (id=0). Default is id=1 verbatim.
bool g_enablePhyIvp = false;

int main(int argc, char** argv)
{

	printf("rmdlconv - Copyright (c) %s, rexx\n", &__DATE__[7]);
	fflush(stdout);

	// Debug: print all arguments
	printf("[DEBUG] argc = %d\n", argc);
	for (int i = 0; i < argc; i++)
	{
		printf("[DEBUG] argv[%d] = '%s'\n", i, argv[i]);
	}
	fflush(stdout);

	CommandLine cmdline(argc, argv);

	// Opt-in to auto-generated BVH4 for v160 -> v8 conversions whose source
	// lacks inline BVH. See g_enableAutoGenBVH declaration above for rationale.
	if (cmdline.HasParam("-autogenbvh"))
	{
		g_enableAutoGenBVH = true;
		printf("[INFO] -autogenbvh enabled: v160 static props without inline BVH "
			"will get an auto-generated BVH4 from their .vg LOD0 mesh.\n");
		fflush(stdout);
	}

	// Opt-in to the legacy id=0 Valve/IVP phy re-encode. Default is id=1 geoms
	// verbatim -- see g_enablePhyIvp declaration above for the reference.
	if (cmdline.HasParam("-phy_ivp"))
	{
		g_enablePhyIvp = true;
		printf("[INFO] -phy_ivp enabled: v16 -> v8 PHY conversion will re-encode "
			"as legacy id=0 Valve/IVP instead of the default id=1 geoms verbatim.\n");
		fflush(stdout);
	}

    if (argc < 2)
        Error("invalid usage\n");

	// Check for batch conversion flags
	const char* batchVersionFlags[] = { "-v8", "-v49", "-vp2", "-v121", "-v122", "-v123", "-v124", "-v125", "-v13", "-v131", "-v14", "-v141", "-v15", "-v16", "-v17", "-v18", "-v19", "-v191", nullptr };
	const char* batchVersionValues[] = { "8", "49", "49", "12.1", "12.2", "12.3", "12.4", "12.5", "13", "13.1", "14", "14.1", "15", "16", "17", "18", "19", "19.1", nullptr };

	printf("[DEBUG] Checking batch flags...\n");
	fflush(stdout);

	for (int i = 0; batchVersionFlags[i] != nullptr; i++)
	{
		if (cmdline.HasParam(batchVersionFlags[i]))
		{
			printf("[DEBUG] Found batch flag: %s\n", batchVersionFlags[i]);
			fflush(stdout);

			// Find the input folder (next arg after the flag)
			// Note: FindParam returns actual argv index, so use argv directly
			int flagIdx = cmdline.FindParam((char*)batchVersionFlags[i]);
			printf("[DEBUG] flagIdx = %d\n", flagIdx);
			fflush(stdout);

			if (flagIdx < 0 || flagIdx + 1 >= argc)
			{
				printf("%s", pszBatchHelpString);
				Error("Missing input folder for batch conversion\n");
			}

			// Use argv directly since FindParam returns actual argv index
			std::string inputFolder = argv[flagIdx + 1];
			printf("[DEBUG] inputFolder = '%s'\n", inputFolder.c_str());
			fflush(stdout);

			std::string outputFolder;

			// Check if output folder is specified (next arg after input folder)
			if (flagIdx + 2 < argc && argv[flagIdx + 2][0] != '-')
			{
				outputFolder = argv[flagIdx + 2];
			}
			else
			{
				// Default output folder
				outputFolder = inputFolder + "_rmdlconv_out";
			}

			printf("[DEBUG] outputFolder = '%s'\n", outputFolder.c_str());
			printf("[DEBUG] Calling BatchConvertModels with version '%s'\n", batchVersionValues[i]);
			fflush(stdout);

			const int batchTarget = cmdline.HasParam("-targetversion")
					? atoi(cmdline.GetParamValue("-targetversion")) : 0;
				BatchConvertModels(batchVersionValues[i], inputFolder, outputFolder, batchTarget);

			if (!cmdline.HasParam("-nopause"))
				std::system("pause");

			return 0;
		}
	}

	// Check for help flag
	if (cmdline.HasParam("-help") || cmdline.HasParam("--help") || cmdline.HasParam("-h") || cmdline.HasParam("-?"))
	{
		printf("%s", pszBatchHelpString);
		return 0;
	}

	if (cmdline.HasParam("-convertmodel"))
	{
		if (!cmdline.HasParam("-targetversion"))
			Error("no '-targetversion' param found while trying to convert model(s)!!!\n required for proper conversion, exiting...\n");

		std::string modelPath = cmdline.GetParamValue("-convertmodel");
		int modelVersionTarget = atoi(cmdline.GetParamValue("-targetversion"));

		const char* customDir = nullptr; // custom base folder for models

		if (cmdline.HasParam("-outputdir"))
			customDir = cmdline.GetParamValue("-outputdir");

		// Optional: specify a custom collision model (SMD file)
		// If not specified, auto-detection is used (modelname_phys.smd, etc.)
		if (cmdline.HasParam("-collisionmodel"))
		{
			const char* collisionPath = cmdline.GetParamValue("-collisionmodel");
			collision::SetCollisionModelPath(collisionPath);
			printf("Using custom collision model: %s\n", collisionPath);
		}

		// Optional: specify source version for Apex Legends models
		// Use -sourceversion 19.1 for Season 19+ models, or -sourceversion 16 for Season 16-18
		if (cmdline.HasParam("-sourceversion"))
		{
			const char* sourceVersion = cmdline.GetParamValue("-sourceversion");
			if ((strcmp(sourceVersion, "12.2") == 0 || strcmp(sourceVersion, "122") == 0) && modelVersionTarget == 17)
			{
				// S10 (mdl_ v12.2) -> S21 client (v17). Full OLD->NEW rebuild + sibling .vg/.phy.
				// Shared with the `-v122 -targetversion 17` batch via ConvertClientModel_122To17.
				if (!std::filesystem::exists(modelPath))
					Error("couldn't find input file: %s\n", modelPath.c_str());

				const std::string pathOut = customDir
					? std::string(customDir) + "/" + std::filesystem::path(modelPath).filename().string()
					: modelPath;
				ConvertClientModel_122To17(modelPath, pathOut);

				collision::ClearCollisionModelPath();
				return 0;
			}
			else if ((strcmp(sourceVersion, "8") == 0 || strcmp(sourceVersion, "v8") == 0) && modelVersionTarget == 17)
			{
				// S3-legacy rmdl v8 -> S21 client v17.
				if (!std::filesystem::exists(modelPath))
					Error("couldn't find input file: %s\n", modelPath.c_str());

				const std::string pathOut = customDir
					? std::string(customDir) + "/" + std::filesystem::path(modelPath).filename().string()
					: modelPath;
				if (customDir)
					std::filesystem::create_directories(std::filesystem::path(pathOut).parent_path());
				ConvertClientModel_8To17(modelPath, pathOut);

				collision::ClearCollisionModelPath();
				return 0;
			}
			else if ((strcmp(sourceVersion, "49") == 0 || strcmp(sourceVersion, "p2") == 0 ||
					  strcmp(sourceVersion, "portal2") == 0) && modelVersionTarget == 17)
			{
				// Portal 2 MDL v49 -> S21 client v17 (via v8 intermediate).
				if (!std::filesystem::exists(modelPath))
					Error("couldn't find input file: %s\n", modelPath.c_str());

				std::string outName = std::filesystem::path(modelPath).filename().string();
				if (EndsWith(outName, ".mdl") || EndsWith(outName, ".MDL"))
					outName = outName.substr(0, outName.length() - 4) + ".rmdl";
				const std::string pathOut = customDir
					? std::string(customDir) + "/" + outName
					: (std::filesystem::path(modelPath).parent_path() / outName).string();
				if (customDir)
					std::filesystem::create_directories(std::filesystem::path(pathOut).parent_path());
				ConvertClientModel_49To17(modelPath, pathOut);

				collision::ClearCollisionModelPath();
				return 0;
			}
			else if (strcmp(sourceVersion, "16") == 0 || strcmp(sourceVersion, "17") == 0 || strcmp(sourceVersion, "18") == 0 || strcmp(sourceVersion, "19") == 0)
			{
				// Handle v16/v17/v18/v19 (Season 13-19) conversion directly
				if (!std::filesystem::exists(modelPath))
					Error("couldn't find input file: %s\n", modelPath.c_str());

				uintmax_t fileSize = GetFileSize(modelPath);
				std::unique_ptr<char[]> pMDL(new char[fileSize]);

				BinaryIO studioModel;
				studioModel.open(modelPath, BinaryIOMode::Read);
				studioModel.getReader()->read(pMDL.get(), fileSize);
				studioModel.close();

				std::string pathOut = customDir ? std::string(customDir) + "/" + std::filesystem::path(modelPath).filename().string() : modelPath;
				if (customDir)
					std::filesystem::create_directories(std::filesystem::path(pathOut).parent_path());

				int subver = std::atoi(sourceVersion);
				ConvertRMDL160To10(pMDL.get(), fileSize, modelPath, pathOut, subver);

				collision::ClearCollisionModelPath();
				return 0;
			}
			else if (strcmp(sourceVersion, "19.1") == 0 || strcmp(sourceVersion, "19") == 0)
			{
				// Handle v19.1 conversion directly
				if (!std::filesystem::exists(modelPath))
					Error("couldn't find input file: %s\n", modelPath.c_str());

				uintmax_t fileSize = GetFileSize(modelPath);
				std::unique_ptr<char[]> pMDL(new char[fileSize]);

				BinaryIO studioModel;
				studioModel.open(modelPath, BinaryIOMode::Read);
				studioModel.getReader()->read(pMDL.get(), fileSize);
				studioModel.close();

				std::string pathOut = customDir ? std::string(customDir) + "/" + std::filesystem::path(modelPath).filename().string() : modelPath;
				if (customDir)
					std::filesystem::create_directories(std::filesystem::path(pathOut).parent_path());

				// -targetversion 17 = v19.1->v17 compact downgrade (keeps v19.1 shape);
				// any other target falls back to the legacy v8/v54 (subversion 10) path.
				if (modelVersionTarget == 17)
					ConvertRMDL191To17(pMDL.get(), fileSize, modelPath, pathOut);
				else
					ConvertRMDL191To10(pMDL.get(), fileSize, modelPath, pathOut);

				collision::ClearCollisionModelPath();
				return 0;
			}
		}

		UpgradeStudioModel(modelPath, modelVersionTarget, customDir);

		// Clear collision path after conversion
		collision::ClearCollisionModelPath();
	}

	if (cmdline.HasParam("-convertsequence"))
	{
		// todo
	}

	LegacyConversionHandling(cmdline); // this should be cut eventually

	if(!cmdline.HasParam("-nopause"))
		std::system("pause");

	return 0;
}
