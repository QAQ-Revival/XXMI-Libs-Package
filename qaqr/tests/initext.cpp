// Offline check that DecodeIniText() and ForEachIniLine() split a file into
// exactly the lines the wifstream they replaced did: text mode CR LF and
// Ctrl+Z handling, the byte order mark, and MSVC's codecvt_utf8 decoding,
// including how it stops at invalid or cut off sequences. Compares both on
// hand picked cases and on random bytes drawn mostly from those that matter.
#define _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <codecvt>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <locale>
#include <random>
#include <share.h>
#include <string>
#include <vector>
#include "../../DirectX11/IniText.h"
namespace fs = std::filesystem;

static std::vector<std::wstring> OldLines(const fs::path &file)
{
	std::vector<std::wstring> lines;
	std::wifstream f(file, std::ios::in, _SH_DENYNO);
	std::wstring line;

	f.imbue(std::locale(f.getloc(), new std::codecvt_utf8<wchar_t, 0x10ffff, std::consume_header>));
	while (std::getline(f, line))
		lines.push_back(line);
	return lines;
}

static std::vector<std::wstring> NewLines(const std::string &bytes)
{
	std::vector<std::wstring> lines;
	std::string copy(bytes);
	std::wstring text;

	DecodeIniText(copy, &text);
	ForEachIniLine(text, [&](const wchar_t *line, size_t len) {
		lines.emplace_back(line, len);
		return true;
	});
	return lines;
}

static std::string Hex(const std::string &bytes)
{
	static const char digits[] = "0123456789abcdef";
	std::string out;
	for (unsigned char c : bytes) {
		out += digits[c >> 4];
		out += digits[c & 15];
		out += ' ';
	}
	return out;
}

int wmain(int argc, wchar_t **argv)
{
	fs::path dir = fs::temp_directory_path() / ("qaqr-initext-" + std::to_string(GetCurrentProcessId()));
	fs::create_directories(dir);
	fs::path file = dir / "case.ini";
	unsigned cases = 0;

	std::vector<std::string> fixed = {
		"", "\n", "\n\n", "a", "a\n", "a\r\nb", "a\rb\r\n", "a\r", "\r\n\r\n", "\r\r\n",
		"\xef\xbb\xbf[Key]\r\nkey = 1\r\n", "\xef\xbb\xbf", "\xef\xbb", "\xef\xbb\xbf\xef\xbb\xbf" "x",
		"x\xef\xbb\xbfy", "a\x1a" "b\nc", "\x1a", "a\r\x1a", "caf\xc3\xa9 = 1\n", "\xe4\xb8\xad\xe6\x96\x87",
		"emoji \xf0\x9f\x98\x80 here\nnext", "bad \x80 byte\nnext", "cut \xe4\xb8", "cut \xe4\xb8\nnext",
		"lead \xc3x\n", "overlong \xc0\x80 \xe0\x80\x80\n", "surrogate \xed\xa0\x80\n",
		"five \xf8\x88\x80\x80\x80 six \xfc\x84\x80\x80\x80\x80\n", "big \xf4\x90\x80\x80\n",
		"max \xf4\x8f\xbf\xbf\n", "weof \xef\xbf\xbf here\nnext", "weof \xf0\x9f\xbf\xbf\n", "fffe \xef\xbf\xbe\n",
		"\xfe\xff", std::string("nul \0 inside\n", 13),
	};
	for (const std::string &bytes : fixed) {
		{ std::ofstream out(file, std::ios::binary); out.write(bytes.data(), bytes.size()); }
		if (OldLines(file) != NewLines(bytes)) {
			std::cerr << "FAIL fixed case: " << Hex(bytes) << "\n";
			return 1;
		}
		cases++;
	}

	// Weighted towards line endings, Ctrl+Z, the byte order mark and
	// every class of UTF-8 lead and continuation byte:
	const unsigned char alphabet[] = {
		'a', '=', ' ', '\t', '[', ';', '\r', '\n', '\r', '\n', 0x1a, 0x00,
		0xef, 0xbb, 0xbf, 0x80, 0xbf, 0x9f, 0xc0, 0xc3, 0xa9, 0xdf,
		0xe0, 0xe4, 0xb8, 0xad, 0xed, 0xa0, 0xf0, 0xf4, 0x8f, 0x90,
		0xf7, 0xf8, 0xfb, 0xfc, 0xfd, 0xff,
	};
	std::mt19937 rng(argc > 1 ? (unsigned)_wtoi(argv[1]) : 20261108u);
	std::uniform_int_distribution<size_t> pick(0, sizeof(alphabet) - 1), length(0, 40);
	std::uniform_int_distribution<int> coin(0, 3);
	for (unsigned n = 0; n < 20000; n++) {
		std::string bytes;
		if (coin(rng) == 0)
			bytes = "\xef\xbb\xbf";
		for (size_t i = length(rng); i; i--)
			bytes += (char)alphabet[pick(rng)];
		{ std::ofstream out(file, std::ios::binary); out.write(bytes.data(), bytes.size()); }
		if (OldLines(file) != NewLines(bytes)) {
			std::cerr << "FAIL random case " << n << ": " << Hex(bytes) << "\n";
			return 1;
		}
		cases++;
	}

	std::error_code ignored;
	fs::remove_all(dir, ignored);
	std::cout << "PASS initext cases=" << cases << "\n";
	return 0;
}
