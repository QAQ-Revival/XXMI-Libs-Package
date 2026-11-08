#pragma once

// Turning the bytes of an ini file into the lines the parser reads. Kept on
// its own for qaqr/tests/initext.cpp, which checks it against the wifstream
// it replaced.

#include <string>

// Decodes an ini file into the text the wifstream that used to read it
// delivered (a codecvt_utf8<wchar_t, 0x10ffff, consume_header> facet on a
// stream opened in text mode), which read a byte at a time through fgetc()
// and was the slowest part of loading a big config. That stream:
// - turned CR LF into LF and ended the file at a Ctrl+Z, as text mode does,
// - dropped a byte order mark at the start,
// - decoded as MSVC's codecvt_utf8 does: lead bytes up to 0xff take up to five
//   continuation bytes, overlong forms and surrogates are not checked for,
//   and code points above U+FFFF are truncated to 16 bits,
// - ended at the first sequence that failed to decode, was cut off by the end
//   of the file, or was above U+10FFFF, keeping everything before it,
// - and also at a character that truncated to U+FFFF, which is WEOF.
// The bytes are modified in place.
inline void DecodeIniText(std::string &bytes, std::wstring *text)
{
	size_t n = 0;

	for (size_t i = 0; i < bytes.size(); i++) {
		if (bytes[i] == '\x1a')
			break;
		if (bytes[i] == '\r' && i + 1 < bytes.size() && bytes[i + 1] == '\n')
			continue;
		bytes[n++] = bytes[i];
	}

	const unsigned char *p = (const unsigned char*)bytes.data(), *end = p + n;
	bool first = true;

	text->clear();
	text->reserve(n);
	while (p < end) {
		unsigned long by = *p, ch;
		int extra, i;

		if (by < 0x80) {
			text->push_back((wchar_t)by);
			p++;
			first = false;
			continue;
		} else if (by < 0xc0) {
			break;
		} else if (by < 0xe0) {
			ch = by & 0x1f;
			extra = 1;
		} else if (by < 0xf0) {
			ch = by & 0x0f;
			extra = 2;
		} else if (by < 0xf8) {
			ch = by & 0x07;
			extra = 3;
		} else {
			ch = by & 0x03;
			extra = by < 0xfc ? 4 : 5;
		}
		if (end - p < extra + 1)
			break;
		for (i = 1; i <= extra; i++) {
			by = p[i];
			if (by < 0x80 || by >= 0xc0)
				break;
			ch = ch << 6 | (by & 0x3f);
		}
		if (i <= extra)
			break;
		p += extra + 1;
		if (first) {
			first = false;
			if (ch == 0xfeff)
				continue;
		}
		if (ch > 0x10ffff)
			break;
		// The stream handed out characters as int_type, where one
		// that truncates to U+FFFF is WEOF and ended the file:
		if ((wchar_t)ch == 0xffff)
			break;
		text->push_back((wchar_t)ch);
	}
}

// Every line up to a newline, and whatever follows the last one, as
// std::getline() splits a stream. Stops early once parse_line returns false.
template <class ParseLine>
inline void ForEachIniLine(const std::wstring &text, ParseLine parse_line)
{
	size_t start = 0, end;

	while (start < text.size()) {
		end = text.find(L'\n', start);
		if (end == std::wstring::npos)
			end = text.size();
		if (!parse_line(text.data() + start, end - start))
			return;
		start = end + 1;
	}
}
