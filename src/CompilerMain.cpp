#include "Basic.h"

#include "CompilerMain.h"
#include "Parser.h"
#include "Infer.h"
#include "IrGenerator.h"
#include "CoffWriter.h"
#include "ArraySet.h"
#include "UTF.h"
#include "Lexer.h"
#include "TypeTable.h"

#if BUILD_WINDOWS
bool doColorPrint = false; // False by default, set at startup if color can be enabled
#else
bool doColorPrint = true;
#endif


std::mutex filesMutex;

static ArraySet<FileInfo> files;

bool loadNewFile(String file) {
	// Open the file here and keep it open until we parse it to avoid race condition
	HANDLE handle = createFileUtf8(file, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN); // @Platform

	if (handle == INVALID_HANDLE_VALUE) {
		reportError("Error: failed to open file: %.*s", STRING_PRINTF(file));
		return false;
	}

	BY_HANDLE_FILE_INFORMATION fileInfo;

	if (!GetFileInformationByHandle(handle, &fileInfo)) {
		reportError("Error: failed to read file information for file: %.*s", STRING_PRINTF(file));
		return false; 
	}

	if (fileInfo.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { // Are there any other attributes we should check for?
		reportError("Error: %.*s is a directory, you can only compile files", STRING_PRINTF(file));
		return false;
	}

	FileInfo info;
	info.path = file;
	info.handle = handle;
	info.volumeSerialNumber = fileInfo.dwVolumeSerialNumber;
	info.fileIndex = (static_cast<u64>(fileInfo.nFileIndexHigh) << 32ULL) | static_cast<u64>(fileInfo.nFileIndexLow);
	info.fileUid = static_cast<u32>(files.size());


	{
		ScopeLock fileLock(filesMutex);
		if (!files.add(info)) {
			CloseHandle(handle);
		}
	}

	return true;
}

FileInfo *getFileInfoByUid(u32 fileUid) {
	FileInfo *info;

	{
		ScopeLock fileLock(filesMutex);
		info = &files[fileUid];
	}

	assert(info->fileUid == fileUid);

	return info;
}

Array<FileInfo> getAllFilesNoLock() {
	return files.array;
}

void displayErrorLocation(CodeLocation *start, EndLocation *end) {
	auto info = getFileInfoByUid(start->fileUid);

	char *file = info->data;
	char *errorStart = file + start->locationInMemory;
	char *errorEnd = file + end->locationInMemory;

	do {
		--errorStart;
		assert(errorStart >= file);
	} while (!utf8ByteCount(*errorStart));

	char *lineStart = errorStart;

	while (lineStart != file) {
		--lineStart;

		if (*lineStart == '\n' || *lineStart == '\r') {
			++lineStart;
			break;
		}
	}

	char *lineEnd = errorEnd;

	while (lineEnd != file + info->size) {
		if (*lineEnd == '\n' || *lineEnd == '\r') {
			break;
		}

		++lineEnd;
	}

	String pre = { lineStart, errorStart };
	String error = { errorStart, errorEnd };
	String post = { errorEnd, lineEnd };

	if (doColorPrint) {
		printf("%.*s\x1b[91m%.*s\x1b[0m%.*s\n", STRING_PRINTF(pre), STRING_PRINTF(error), STRING_PRINTF(post));
	}
	else {
		printf("%.*s%.*s%.*s\n", STRING_PRINTF(pre), STRING_PRINTF(error), STRING_PRINTF(post));
	}
}

void printErrorLocation(CodeLocation *location) {
	String filename = getFileInfoByUid(location->fileUid)->path;

	printf("%.*s:%" PRIu32 ",%" PRIu32 " ", STRING_PRINTF(filename), location->line, location->column);

}

void reportError(CHECK_PRINTF const char *format, ...) {
	hadError = true;

	va_list args;
	va_start(args, format);

	vprintf(format, args);

	va_end(args);

}

void reportError(Expr *location, CHECK_PRINTF const char *format, ...) {
	printErrorLocation(&location->start);
	hadError = true;

	va_list args;
	va_start(args, format);

	vprintf(format, args);
	puts("");

	va_end(args);

	displayErrorLocation(&location->start, &location->end);
}



void reportError(CodeLocation *start, EndLocation *end, CHECK_PRINTF const char *format, ...) {
	printErrorLocation(start);
	hadError = true;

	va_list args;
	va_start(args, format);

	vprintf(format, args);
	puts("");

	va_end(args);

	displayErrorLocation(start, end);
}

void reportError(CodeLocation *location, CHECK_PRINTF const char *format, ...) {
	printErrorLocation(location);

	hadError = true;

	va_list args;
	va_start(args, format);

	vprintf(format, args);
	puts("");

	va_end(args);
}


void reportError(Declaration *location, CHECK_PRINTF const char *format, ...) {
	printErrorLocation(&location->start);

	hadError = true;

	va_list args;
	va_start(args, format);

	vprintf(format, args);
	puts("");

	va_end(args);

	displayErrorLocation(&location->start, &location->end);
}

void reportError(Token *location, CHECK_PRINTF const char *format, ...) {
	printErrorLocation(&location->start);

	hadError = true;

	va_list args;
	va_start(args, format);

	vprintf(format, args);
	puts("");

	va_end(args);

	displayErrorLocation(&location->start, &location->end);
}

void reportExpectedError(Token *location, CHECK_PRINTF const char *format, ...) {
	if (location->type == TokenT::INVALID) { // If it was invalid assume that the lexer already reported an error, don't print the error so we don't double report
		assert(hadError);
	}
	else {
		printErrorLocation(&location->start);

		hadError = true;

		va_list args;
		va_start(args, format);

		vprintf(format, args);

		va_end(args);

		String token = getTokenString(location);
		printf(" but got %.*s", STRING_PRINTF(token));
		puts("");

		displayErrorLocation(&location->start, &location->end);
	}
}

int main(int argc, char *argv[]) {
#if BUILD_WINDOWS
	{
		HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);
		if (console != INVALID_HANDLE_VALUE) {
			DWORD mode;

			if (GetConsoleMode(console, &mode)) {
				if (SetConsoleMode(console, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
					doColorPrint = true;
				}
			}
		}
	}
#endif

#if BUILD_PROFILE
	u64 startTime;
	QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER *>(&startTime));
#endif

	if (argc != 2) {
		reportError("Error: Expected name of input file");
		reportError("Usage: %s <file>", argv[0]);
	}

	char *input = argv[1];

	auto start = std::chrono::high_resolution_clock::now();

	

	if (!hadError && loadNewFile(String(input))) {
		setupTypeTable();

		std::thread infer(runInfer);
		std::thread irGenerator(runIrGenerator);
		std::thread coffWriter(runCoffWriter);

		mainThread = std::this_thread::get_id();
		inferThread = infer.get_id();

		SetThreadDescription(infer.native_handle(), L"Infer");
		SetThreadDescription(irGenerator.native_handle(), L"Ir Generator");
		SetThreadDescription(coffWriter.native_handle(), L"Coff Writer");

		infer.detach();
		irGenerator.detach();

		{
			u64 i;
			for (i = 0; i < files.size(); i++) {
				parseFile(getFileInfoByUid(static_cast<u32>(i)));

				if (hadError) {
					break;
				}
			}

			inferQueue.add(makeStopSignal());

			for (++i; i < files.size(); i++) { // In the event we had an error so stopped parsing files mid way through
				CloseHandle(files[i].handle);
			}
		}
		coffWriter.join();


	std::cout << "It took me " << (std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::duration<double>(
		std::chrono::high_resolution_clock::now() - start)).count() / 1000.0) << "ms\n";
	}
	
#if BUILD_WINDOWS
	if (!hadError) {
		char buffer[1024];
		
		// use this: https://gist.github.com/andrewrk/ffb272748448174e6cdb4958dae9f3d8
		strcpy_s(buffer, 
#include "LinkCommand.h"
		);

		printf("Linker command: %s\n", buffer);

		STARTUPINFOA startup = {};
		startup.cb = sizeof(STARTUPINFOA);

		PROCESS_INFORMATION info;

		if (!CreateProcessA(NULL, buffer, NULL, NULL, false, 0, NULL, NULL, &startup, &info)) {
			std::cout << "Failed to run linker command" << std::endl;
		}

		CloseHandle(info.hThread);

		WaitForSingleObject(info.hProcess, INFINITE);
		CloseHandle(info.hProcess);
	}
#else
// @Platform
#error "Non windows builds are not supported" 
#endif

#if BUILD_PROFILE
	{
		std::ofstream out("profile.json", std::ios::out | std::ios::trunc);

		u64 pcf;
		QueryPerformanceFrequency(reinterpret_cast<LARGE_INTEGER *>(&pcf));

		out << '[';
		for (u32 i = 0; i < static_cast<u32>(profileIndex); i++) {
			Profile p = profiles[i];

			out << "{\"cat\":\"function\",\"pid\":0,\"tid\":" << p.threadId << ",\"ts\":" << ((p.time - startTime) * 1.0e9 / (double) pcf);

			if (p.name) {
				out << ",\"ph\":\"B\",\"name\":\"" << p.name;

				if (p.data) {
					out << "\",\"args\":{\"data\":\"" << p.data << "\"}}";
				}
				else {
					out << "\"}";
				}
			}
			else {
				out << ",\"ph\":\"E\"}";
			}

			if (i != static_cast<u32>(profileIndex) - 1) {
				out << ",\n";
			}

		}

		out << ']';
	}

#endif



#if BUILD_DEBUG
	std::cin.get();
#endif

	return hadError;
}

