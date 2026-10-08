#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>

// Translations for GUI strings, read from <dir>/<lang>.lang (UTF-8 "key=value" lines).
// Every language is layered over en, so missing keys fall back to en. Thread safe.
namespace i18n {

	using table = std::unordered_map<std::string, std::string>;

	// Returns false when <lang>.lang or en.lang couldn't be read.
	bool set(std::string_view lang);

	// Returns the key itself when no language has it, so the gap shows up on screen.
	std::string get(std::string_view key);

	std::string current();

	// Defaults to <exe dir>/lang.
	void set_dir(std::filesystem::path dir);

	// "ja" for a Japanese UI, otherwise "en".
	std::string system_language();

	namespace detail {

		// Skips blank and '#' lines, trims both sides, splits at the first '=', and turns "\n" into a newline.
		table parse(std::string_view text);

		void apply(std::string_view lang, std::string_view en_text, std::string_view lang_text);
	}
}
