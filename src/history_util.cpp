#include "history_util.h"

#include <string>

namespace config {

namespace {

// ASCII whitespace, matching what the API treats as blank.
const char* const kBlank = " \t\r\n\f\v";

// Placeholder for a message whose content would otherwise be empty.
const char* const kNoContent = "(no content)";

// Placeholder for a tool that produced no output at all.
const char* const kNoOutput = "(no output)";

bool IsBlank(const std::string& s) {
	return s.find_first_not_of(kBlank) == std::string::npos;
}

// Ensure a tool_result block carries something the API will accept. Its
// content may be a plain string or an array of blocks; both forms fall
// back to the "(no output)" marker when nothing survives.
void RepairToolResult(json& block) {
	auto it = block.find("content");
	if (it == block.end() || it->is_null()) {
		block["content"] = kNoOutput;
		return;
	}
	if (it->is_string()) {
		if (IsBlank(it->get<std::string>())) *it = kNoOutput;
		return;
	}
	if (!it->is_array()) return;

	json kept = json::array();
	for (const auto& inner : *it) {
		if (inner.is_object() && inner.value("type", "") == "text"
		    && IsBlank(inner.value("text", std::string{})))
			continue;
		kept.push_back(inner);
	}
	if (kept.empty())
		kept.push_back({{"type", "text"}, {"text", kNoOutput}});
	*it = std::move(kept);
}

} // namespace

json CapHistoryMessages(const json& messages, size_t cap) {
	if (!messages.is_array() || messages.size() <= cap)
		return messages;
	size_t start = messages.size() - cap;
	auto leadsWithToolResult = [](const json& msg) -> bool {
		if (msg.value("role", "") != "user") return false;
		if (!msg.contains("content") || !msg["content"].is_array()) return false;
		if (msg["content"].empty()) return false;
		return msg["content"][0].value("type", "") == "tool_result";
	};
	while (start < messages.size() && leadsWithToolResult(messages[start]))
		++start;
	json capped = json::array();
	for (size_t i = start; i < messages.size(); ++i)
		capped.push_back(messages[i]);
	return capped;
}

json RepairEmptyTextBlocks(const json& messages) {
	if (!messages.is_array()) return messages;

	json out = json::array();
	for (const auto& msg : messages) {
		if (!msg.is_object() || !msg.contains("content")) {
			out.push_back(msg);
			continue;
		}

		json m = msg;
		json& content = m["content"];

		if (content.is_string()) {
			if (IsBlank(content.get<std::string>())) content = kNoContent;
		} else if (content.is_array()) {
			json kept = json::array();
			for (const auto& block : content) {
				if (!block.is_object()) {
					kept.push_back(block);
					continue;
				}
				const std::string type = block.value("type", "");
				if (type == "text") {
					// Drop the block entirely; a sibling tool_use or
					// image usually carries the real payload.
					if (IsBlank(block.value("text", std::string{})))
						continue;
					kept.push_back(block);
				} else if (type == "tool_result") {
					json repaired = block;
					RepairToolResult(repaired);
					kept.push_back(std::move(repaired));
				} else {
					kept.push_back(block);
				}
			}
			if (kept.empty())
				kept.push_back({{"type", "text"}, {"text", kNoContent}});
			content = std::move(kept);
		} else if (content.is_null()) {
			content = kNoContent;
		}

		out.push_back(std::move(m));
	}
	return out;
}

} // namespace config
