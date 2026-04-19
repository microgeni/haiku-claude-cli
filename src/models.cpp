#include "models.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>

#include <curl/curl.h>

#include "tui.h"

namespace models {

std::vector<ModelEntry> FetchModels(const config::Auth& auth) {
	CURL* curl = curl_easy_init();
	if (!curl) return {};

	std::string body;
	auto write_cb = [](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
		auto* s = static_cast<std::string*>(userdata);
		s->append(ptr, size * nmemb);
		return size * nmemb;
	};

	curl_slist* headers = nullptr;
	if (auth.kind == config::AuthKind::OAuth) {
		headers = curl_slist_append(headers, ("authorization: Bearer " + auth.credential).c_str());
		headers = curl_slist_append(headers, (std::string("anthropic-beta: ") + config::kOAuthBeta).c_str());
	} else {
		headers = curl_slist_append(headers, ("x-api-key: " + auth.credential).c_str());
	}
	headers = curl_slist_append(headers, (std::string("anthropic-version: ") + config::kApiVersion).c_str());

	curl_easy_setopt(curl, CURLOPT_URL, "https://api.anthropic.com/v1/models?limit=100");
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, static_cast<size_t(*)(char*,size_t,size_t,void*)>(write_cb));
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);

	const CURLcode res = curl_easy_perform(curl);
	long http_status = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	if (res != CURLE_OK || http_status != 200) return {};

	std::vector<ModelEntry> out;
	try {
		const auto j = json::parse(body);
		if (!j.contains("data") || !j["data"].is_array()) return {};
		for (const auto& m : j["data"]) {
			ModelEntry e;
			e.id           = m.value("id", "");
			e.display_name = m.value("display_name", e.id);
			if (!e.id.empty()) out.push_back(std::move(e));
		}
	} catch (...) {}
	return out;
}

int DetectContextWindow(const std::string& model, int override_val) {
	if (override_val > 0) return override_val;
	if (model.find("[1m]") != std::string::npos) return 1'000'000;
	return 200'000;
}

PriceEntry GetPrice(const std::string& model, const json& config_prices) {
	if (config_prices.is_object() && config_prices.contains(model)) {
		const auto& p = config_prices[model];
		return { p.value("input", 0.0), p.value("output", 0.0) };
	}
	// Per-million-token fallbacks based on publicly listed Claude pricing.
	if (model.find("opus")   != std::string::npos) return { 15.0, 75.0 };
	if (model.find("haiku")  != std::string::npos) return { 0.8,   4.0 };
	if (model.find("sonnet") != std::string::npos) return { 3.0,  15.0 };
	return { 3.0, 15.0 };
}

std::string CompactTokens(int n) {
	if (n < 1000) return std::to_string(n);
	if (n < 10000) {
		char buf[16];
		std::snprintf(buf, sizeof(buf), "%.1fk", n / 1000.0);
		return buf;
	}
	if (n < 1000000) {
		char buf[16];
		std::snprintf(buf, sizeof(buf), "%dk", n / 1000);
		return buf;
	}
	char buf[16];
	std::snprintf(buf, sizeof(buf), "%.1fM", n / 1000000.0);
	return buf;
}

std::string FormatStatusRow(const std::string& model,
                            int turn_count,
                            int session_input,
                            int session_output,
                            int max_tokens,
                            const std::string& right_label) {
	auto short_model = [](const std::string& m) {
		const std::string prefix = "claude-";
		if (m.rfind(prefix, 0) == 0) return m.substr(prefix.size());
		return m;
	};

	std::string left;
	left.reserve(96);
	left += " ";
	left += tui::Bold(short_model(model));
	left += tui::Muted(" \xC2\xB7 turn ") + tui::Muted(std::to_string(turn_count));
	left += tui::Muted(" \xC2\xB7 \xE2\x86\x91 ") + tui::Muted(CompactTokens(session_input));
	left += tui::Muted(" \xC2\xB7 \xE2\x86\x93 ") + tui::Muted(CompactTokens(session_output));
	left += tui::Muted(" \xC2\xB7 max ") + tui::Muted(std::to_string(max_tokens));

	if (right_label.empty()) return left;

	const int width = tui::TerminalWidth();
	if (width <= 0) return left + "  " + right_label;

	const int left_cols  = tui::DisplayWidth(left);
	const int right_cols = tui::DisplayWidth(right_label);
	const int gap        = (width - 1) - left_cols - right_cols;
	if (gap < 2) return left + "  " + right_label;
	return left + std::string(gap, ' ') + right_label + " ";
}

std::vector<std::pair<std::string, std::string>>
ExtractNumberedOptions(const std::string& text) {
	std::vector<std::pair<std::string, std::string>> out;
	std::istringstream iss(text);
	std::string        line;
	while (std::getline(iss, line)) {
		size_t i = 0;
		while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i]))) ++i;
		if (i == 0 || i > 2)                            continue; // need 1-2 digits
		if (i >= line.size() || line[i] != '.')         continue;
		if (i + 1 >= line.size() || line[i + 1] != ' ') continue;
		std::string number = line.substr(0, i);
		std::string label  = line.substr(i + 2);
		if (label.size() > 28) {
			label.resize(27);
			label += "\xE2\x80\xA6"; // …
		}
		out.emplace_back(std::move(number), std::move(label));
	}
	if (out.size() < 2) return {};
	return out;
}

} // namespace models
