#include "history_util.h"

namespace config {

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

} // namespace config
