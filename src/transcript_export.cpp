#include "transcript_export.h"

namespace transcript {

using nlohmann::json;

namespace {

// Render one content value (plain string or block array) to Markdown.
std::string RenderContent(const json& content) {
	std::string s;
	if (content.is_string()) {
		s = content.get<std::string>();
	} else if (content.is_array()) {
		for (const auto& block : content) {
			const std::string type = block.value("type", "");
			if (type == "text") {
				s += block.value("text", "");
			} else if (type == "image") {
				const std::string mt =
					block.contains("source")
						? block["source"].value("media_type", "image")
						: "image";
				s += "_[image attachment: " + mt + "]_";
			} else if (type == "tool_use") {
				s += "\n> \xF0\x9F\x94\xA7 **tool call:** `"
				   + block.value("name", "?") + "`\n";
			} else if (type == "tool_result") {
				std::string rc;
				const json& c = block.contains("content")
					? block["content"] : json();
				if (c.is_string()) {
					rc = c.get<std::string>();
				} else if (c.is_array()) {
					for (const auto& cb : c)
						if (cb.value("type", "") == "text")
							rc += cb.value("text", "");
				}
				if (rc.size() > 1000) { rc.resize(1000); rc += "\n\xE2\x80\xA6[truncated]"; }
				s += "\n> \xF0\x9F\x94\xA7 **tool result:**\n```\n" + rc + "\n```\n";
			}
		}
	}
	return s;
}

} // namespace

std::string ToMarkdown(const std::string& topic,
                       const std::string& model,
                       int turns,
                       const json& messages) {
	std::string out;
	out += "# Claude transcript\n\n";
	if (!topic.empty()) out += "**Topic:** " + topic + "\n\n";
	out += "**Model:** " + model + "  \n";
	out += "**Turns:** " + std::to_string(turns) + "\n\n---\n\n";

	if (!messages.is_array()) return out;

	for (const auto& turn : messages) {
		const std::string role = turn.value("role", "");
		if (!turn.contains("content")) continue;
		const json& content = turn["content"];

		// A user turn whose content is purely tool_result blocks is the
		// automated half of a tool round-trip, not something the human
		// typed — label it as such so the transcript reads correctly.
		bool toolResultOnly = false;
		if (role == "user" && content.is_array() && !content.empty()) {
			toolResultOnly = true;
			for (const auto& b : content)
				if (b.value("type", "") != "tool_result") { toolResultOnly = false; break; }
		}

		const std::string body = RenderContent(content);
		if (body.empty()) continue;
		if (toolResultOnly)
			out += "## Tool result\n\n" + body + "\n\n";
		else if (role == "user")
			out += "## You\n\n" + body + "\n\n";
		else if (role == "assistant")
			out += "## Claude\n\n" + body + "\n\n";
		else
			out += "## " + role + "\n\n" + body + "\n\n";
	}

	return out;
}

} // namespace transcript
