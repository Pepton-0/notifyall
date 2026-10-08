#include "i18n.h"

#include <format>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>

#include <windows.h>

#include "logger.h"

namespace {

	std::mutex m;
	i18n::table entries;
	std::string lang_now = "en";
	std::filesystem::path lang_dir;

	std::filesystem::path default_dir()
	{
		wchar_t path[MAX_PATH]{};
		const DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
		if (len == 0 || len == MAX_PATH) return L"lang";
		return std::filesystem::path(std::wstring_view(path, len)).parent_path() / L"lang";
	}

	std::optional<std::string> read_all(const std::filesystem::path& file)
	{
		std::ifstream in(file, std::ios::binary);
		if (!in) return std::nullopt;

		std::ostringstream ss;
		ss << in.rdbuf();
		return ss.str();
	}

	std::string_view trim(std::string_view s)
	{
		constexpr std::string_view space = " \t\r\n";
		const auto b = s.find_first_not_of(space);
		if (b == std::string_view::npos) return {};
		const auto e = s.find_last_not_of(space);
		return s.substr(b, e - b + 1);
	}

	std::string unescape(std::string_view s)
	{
		std::string out;
		out.reserve(s.size());
		for (std::size_t i = 0; i < s.size(); ++i) {
			if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 'n') { out += '\n'; ++i; }
			else out += s[i];
		}
		return out;
	}

}

namespace i18n {

	bool set(std::string_view lang)
	{
		std::filesystem::path dir;
		{
			const std::lock_guard lock(m);
			if (lang_dir.empty()) lang_dir = default_dir();
			dir = lang_dir;
		}

		const auto en = read_all(dir / L"en.lang");
		if (!en) lib::log("couldn't read en.lang", true);

		std::optional<std::string> other;
		if (lang != "en") {
			other = read_all(dir / std::format("{}.lang", lang));
			if (!other) lib::log(std::format("couldn't read {}.lang. falls back to en.", lang), true);
		}

		const bool ok = en && (lang == "en" || other);
		detail::apply(lang == "en" || other ? lang : "en", en.value_or(""), other.value_or(""));
		lib::log(std::format("language: {}", current()));
		return ok;
	}

	std::string get(std::string_view key)
	{
		const std::lock_guard lock(m);
		if (const auto itr = entries.find(std::string(key)); itr != entries.end()) return itr->second;
		return std::string(key);
	}

	std::string current()
	{
		const std::lock_guard lock(m);
		return lang_now;
	}

	void set_dir(std::filesystem::path dir)
	{
		const std::lock_guard lock(m);
		lang_dir = std::move(dir);
	}

	std::string system_language()
	{
		return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_JAPANESE ? "ja" : "en";
	}

	namespace detail {

		table parse(std::string_view text)
		{
			constexpr std::string_view utf8_bom = "\xEF\xBB\xBF";
			if (text.starts_with(utf8_bom)) text.remove_prefix(utf8_bom.size());

			table t;
			while (!text.empty()) {
				const auto nl = text.find('\n');
				const auto line = trim(text.substr(0, nl));
				text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);

				if (line.empty() || line.front() == '#') continue;

				const auto eq = line.find('=');
				if (eq == std::string_view::npos) continue;

				const auto key = trim(line.substr(0, eq));
				if (key.empty()) continue;

				t.insert_or_assign(std::string(key), unescape(trim(line.substr(eq + 1))));
			}
			return t;
		}

		void apply(std::string_view lang, std::string_view en_text, std::string_view lang_text)
		{
			table t = parse(en_text);
			for (auto& [k, v] : parse(lang_text)) t.insert_or_assign(k, std::move(v));

			const std::lock_guard lock(m);
			entries = std::move(t);
			lang_now = std::string(lang);
		}
	}
}
